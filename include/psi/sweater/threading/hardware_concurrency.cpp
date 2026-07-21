////////////////////////////////////////////////////////////////////////////////
///
/// \file hardware_concurrency.cpp
/// ------------------------------
///
/// (c) Copyright Domagoj Saric 2016 - 2023.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "hardware_concurrency.hpp"
//------------------------------------------------------------------------------
#ifdef __ANDROID__
#   include <android/api-level.h>
#   include <unistd.h>
#endif // __ANDROID__
#ifdef __EMSCRIPTEN_PTHREADS__
#   include <emscripten/threading.h>
#endif // __EMSCRIPTEN_PTHREADS__
#ifdef __linux__
#   include <sys/sysinfo.h>
#endif // __linux__

#if PSI_SWEATER_DOCKER_LIMITS
#   include <boost/assert.hpp>

#   include <fcntl.h>
#   include <sys/types.h>
#   include <unistd.h>

#   include <algorithm>
#   include <cstddef>
#   include <cstdio>
#   include <cstdlib>
#   include <cstring>
#endif // PSI_SWEATER_DOCKER_LIMITS

#ifdef _MSC_VER
#   include <boost/preprocessor/stringize.hpp> // current MS STL yvals.h no longer provides _STRINGIZE
#   pragma detect_mismatch( "PSI_SWEATER_MAX_HARDWARE_CONCURRENCY", BOOST_PP_STRINGIZE( PSI_SWEATER_MAX_HARDWARE_CONCURRENCY ) )
#endif // _MSC_VER

#include <cstdint>
#include <thread>
//------------------------------------------------------------------------------
#ifdef __ANDROID__
// https://android.googlesource.com/platform/bionic/+/HEAD/docs/status.md
__attribute__(( weak )) int get_nprocs     () noexcept { return static_cast< int >( ::sysconf( _SC_NPROCESSORS_ONLN ) ); }
__attribute__(( weak )) int get_nprocs_conf() noexcept { return static_cast< int >( ::sysconf( _SC_NPROCESSORS_CONF ) ); }
#endif // __ANDROID__
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

#if PSI_SWEATER_MAX_HARDWARE_CONCURRENCY == 1

hardware_concurrency_t hardware_concurrency_current() noexcept { return 1; }
hardware_concurrency_t get_hardware_concurrency_max() noexcept { return 1; }

#elif PSI_SWEATER_DOCKER_LIMITS

namespace
{
    // Enough for a cgroup mount point plus the deepest cgroup path the kernel
    // will hand out in /proc/self/cgroup.
    auto constexpr path_size{ 512 };

    // Reads at most size-1 bytes and NUL terminates. Returns false if the file
    // does not exist (the normal outcome for the cgroup hierarchy this build is
    // not running under) or cannot be read.
    bool read_text( char const * const file_path, char * const buffer, std::size_t const size ) noexcept
    {
        BOOST_ASSERT( size > 1 );
        auto const fd{ ::open( file_path, O_RDONLY, 0 ) };
        if ( fd == -1 )
            return false;
        auto const bytes_read{ ::read( fd, buffer, size - 1 ) };
        BOOST_VERIFY( ::close( fd ) == 0 );
        if ( bytes_read <= 0 )
            return false;
        buffer[ bytes_read ] = '\0';
        return true;
    }

    int read_int( char const * const file_path ) noexcept
    {
        char value[ 64 ];
        if ( !read_text( file_path, value, sizeof( value ) ) )
            return -1;
        return std::atoi( value );
    }

    // Docker allows non-whole core quota assignments - use some sort of
    // heurestical rounding. Yields -1 for "no quota in place".
    int cpus_from_quota( long const quota, long const period ) noexcept
    {
        if ( ( quota <= 0 ) || ( period <= 0 ) )
            return -1;
        return std::max( static_cast<int>( ( quota + period / 2 ) / period ), 1 );
    }

    // cgroup v2: "$MAX $PERIOD" in <dir>/cpu.max, where MAX is the quota in
    // microseconds or the literal "max" for an unconstrained cgroup.
    int read_quota_v2( char const * const dir ) noexcept
    {
        char path[ path_size ];
        std::snprintf( path, sizeof( path ), "%s/cpu.max", dir );
        char value_pair[ 128 ];
        if ( !read_text( path, value_pair, sizeof( value_pair ) ) )
            return -1;
        char * period_ptr;
        auto const quota{ std::strtol( value_pair, &period_ptr, 10 ) };
        if ( period_ptr == value_pair ) // "max"
            return -1;
        return cpus_from_quota( quota, std::strtol( period_ptr, nullptr, 10 ) );
    }

    // cgroup v1: the quota/period pair as separate files.
    int read_quota_v1( char const * const dir ) noexcept
    {
        char path[ path_size ];
        std::snprintf( path, sizeof( path ), "%s/cpu.cfs_quota_us", dir );
        auto const quota { read_int( path ) };
        std::snprintf( path, sizeof( path ), "%s/cpu.cfs_period_us", dir );
        auto const period{ read_int( path ) };
        return cpus_from_quota( quota, period );
    }

    // Walks `dir` up to (and including) its first `root_len` characters,
    // returning the most restrictive quota found along the way. Destroys `dir`.
    int scan_branch( char * const dir, std::size_t const root_len, bool const v2 ) noexcept
    {
        int limit{ -1 };
        for ( ;; )
        {
            auto const quota{ v2 ? read_quota_v2( dir ) : read_quota_v1( dir ) };
            if ( quota > 0 )
                limit = ( limit == -1 ) ? quota : std::min( limit, quota );
            if ( std::strlen( dir ) <= root_len )
                return limit;
            auto * const last_separator{ std::strrchr( dir + root_len, '/' ) };
            if ( !last_separator )
                return limit;
            *last_separator = '\0';
        }
    }

    // `cpu` as a whole entry in a comma separated v1 controller list (`cpuset`
    // and `cpuacct` are different controllers and must not match).
    bool has_cpu_controller( char const * entry, char const * const end ) noexcept
    {
        while ( entry < end )
        {
            auto const * entry_end{ std::strchr( entry, ',' ) };
            if ( !entry_end || ( entry_end > end ) )
                entry_end = end;
            if ( ( ( entry_end - entry ) == 3 ) && ( std::strncmp( entry, "cpu", 3 ) == 0 ) )
                return true;
            entry = entry_end + 1;
        }
        return false;
    }

    // The path of the process's own cgroup within the hierarchy, from
    // /proc/self/cgroup: "0::<path>" for the unified hierarchy, or
    // "<id>:<controllers>:<path>" with `cpu` among the controllers for v1.
    // Yields the empty string for the hierarchy root (and when there is no
    // such line) so that concatenation with the mount point cannot produce
    // a "//".
    void self_cgroup( char const * const proc_self_cgroup, bool const v2, char * const out, std::size_t const size ) noexcept
    {
        out[ 0 ] = '\0';
        for ( auto const * line{ proc_self_cgroup }; line && *line; )
        {
            auto const * const eol     { std::strchr( line, '\n' ) };
            auto const * const line_end{ eol ? eol : ( line + std::strlen( line ) ) };
            auto const * const field2  { std::strchr( line, ':' ) };
            auto const * const field3  { ( field2 && ( field2 < line_end ) ) ? std::strchr( field2 + 1, ':' ) : nullptr };
            if ( field3 && ( field3 < line_end ) )
            {
                auto const matches{ v2 ? ( ( field2 + 1 ) == field3 ) : has_cpu_controller( field2 + 1, field3 ) };
                if ( matches )
                {
                    auto const length{ std::min( static_cast<std::size_t>( line_end - field3 - 1 ), size - 1 ) };
                    std::memcpy( out, field3 + 1, length );
                    out[ length ] = '\0';
                    if ( ( length == 1 ) && ( out[ 0 ] == '/' ) ) // the hierarchy root
                        out[ 0 ] = '\0';
                    return;
                }
            }
            line = eol ? ( eol + 1 ) : nullptr;
        }
    }

    auto get_docker_limit() noexcept
    {
        // https://bugs.openjdk.java.net/browse/JDK-8146115
        // http://hg.openjdk.java.net/jdk/hs/rev/7f22774a5f42
        // RAM limit /sys/fs/cgroup/memory.limit_in_bytes
        // swap limt /sys/fs/cgroup/memory.memsw.limit_in_bytes
        // https://github.com/moby/moby/issues/20770#issuecomment-1559152307
        //
        // The quota lives in the cgroup the process actually belongs to, which
        // is the hierarchy ROOT only when a cgroup namespace makes it look that
        // way - the usual container case, and the only one reading
        // /sys/fs/cgroup/cpu.max directly ever covered. In a nested cgroup, a
        // cgroupns=host container or a plain systemd scope, the root carries no
        // quota at all (the v2 root does not even have a cpu.max) and the limit
        // has to be looked up along the process's own branch instead. A quota
        // on any ancestor constrains us too, hence the most restrictive one
        // found wins. Both hierarchies are consulted rather than one being
        // picked up front, since a host can present both.
        char proc_self[ 1024 ];
        if ( !read_text( "/proc/self/cgroup", proc_self, sizeof( proc_self ) ) )
            proc_self[ 0 ] = '\0';

        static constexpr char v2_root[]{ "/sys/fs/cgroup"     };
        static constexpr char v1_root[]{ "/sys/fs/cgroup/cpu" };

        char relative_path[ path_size ];
        char dir          [ path_size + sizeof( v1_root ) ];

        self_cgroup( proc_self, true, relative_path, sizeof( relative_path ) );
        std::snprintf( dir, sizeof( dir ), "%s%s", v2_root, relative_path );
        auto const v2_limit{ scan_branch( dir, sizeof( v2_root ) - 1, true ) };

        self_cgroup( proc_self, false, relative_path, sizeof( relative_path ) );
        std::snprintf( dir, sizeof( dir ), "%s%s", v1_root, relative_path );
        auto const v1_limit{ scan_branch( dir, sizeof( v1_root ) - 1, false ) };

        if ( v2_limit == -1 ) return v1_limit;
        if ( v1_limit == -1 ) return v2_limit;
        return std::min( v2_limit, v1_limit );
    }

    struct docker_quota_t // TODO extract a generalized 'prioritized value wrapper' template
    {
        docker_quota_t() noexcept : value{ get_docker_limit() } {}
        int const value;
        __attribute__(( pure )) operator int() const noexcept { return value; }
    } const docker_quota __attribute__(( init_priority( 101 ) ));
} // anonymous namespace

hardware_concurrency_t get_hardware_concurrency_max() noexcept
{
    // Obey docker limits even when someone attempts to create a pool with
    // more threads than allowed by the Docker container but return the number
    // of all CPUs when there is no Docker CPU quota in place.
    return static_cast<hardware_concurrency_t>( ( docker_quota != -1 ) ? docker_quota : get_nprocs_conf() );
}

hardware_concurrency_t hardware_concurrency_current() noexcept { return static_cast<hardware_concurrency_t>( ( docker_quota != -1 ) ? docker_quota : get_nprocs() ); }

#else // generic/standard impl

hardware_concurrency_t get_hardware_concurrency_max() noexcept
{
    return static_cast<hardware_concurrency_t>
    (
#   if defined( __EMSCRIPTEN_PTHREADS__ )
        emscripten_has_threading_support() ? emscripten_num_logical_cores() : 1
#   elif defined( __linux__ )
        // libcpp std::thread::hardware_concurrency() returns the dynamic number of active cores.
        get_nprocs_conf()
#   else
        std::thread::hardware_concurrency()
#   endif
    );
}

hardware_concurrency_t hardware_concurrency_current() noexcept
{
    return static_cast<hardware_concurrency_t>
    (
#   if defined( __EMSCRIPTEN_PTHREADS__ )
        get_hardware_concurrency_max()
#   elif defined( __linux__ )
        get_nprocs()
#   else
        std::thread::hardware_concurrency()
#   endif
    );
}

#endif // impl

// http://clang-developers.42468.n3.nabble.com/Clang-equivalent-to-attribute-init-priority-td4034229.html
// https://gcc.gnu.org/ml/gcc-help/2011-05/msg00221.html
// "can only use 'init_priority' attribute on file-scope definitions of objects of class type"
hardware_concurrency_max_t const hardware_concurrency_max
#ifdef __GNUC__
 __attribute__(( init_priority( 101 ) ))
#endif
;


#ifdef __ANDROID__
slow_thread_signals_t::slow_thread_signals_t() noexcept
    : value{ android_get_device_api_level() < 24 } {} // pre Android 7 Noughat

slow_thread_signals_t const slow_thread_signals __attribute__(( init_priority( 101 ) ));
#endif // ANDROID

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
