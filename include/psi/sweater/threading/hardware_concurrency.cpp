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
#   include <cstdlib>
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

    auto get_docker_limit() noexcept
    {
        // https://bugs.openjdk.java.net/browse/JDK-8146115
        // http://hg.openjdk.java.net/jdk/hs/rev/7f22774a5f42
        // RAM limit /sys/fs/cgroup/memory.limit_in_bytes
        // swap limt /sys/fs/cgroup/memory.memsw.limit_in_bytes

        // Which of the two cgroup hierarchies is mounted is a property of the
        // host, not of the target architecture: unified (v2) is the default on
        // contemporary distributions on every arch, while v1 is still what
        // older hosts present. Probe v2 first and fall back to v1 - selecting
        // by architecture instead silently misses the quota (and so sizes pools
        // by the host core count) on any v2 host that is not the arch the v2
        // path happened to be written for.
        // https://github.com/moby/moby/issues/20770#issuecomment-1559152307
        long cfs_quota { -1 };
        long cfs_period{ -1 };
        char value_pair[ 128 ];
        if ( read_text( "/sys/fs/cgroup/cpu.max", value_pair, sizeof( value_pair ) ) ) // cgroup v2
        {
            // "$MAX $PERIOD", where MAX is either the quota in microseconds or
            // the literal "max" for an unconstrained cgroup.
            char * cfs_period_ptr;
            cfs_quota  = std::strtol( value_pair    , &cfs_period_ptr, 10 );
            cfs_period = std::strtol( cfs_period_ptr, nullptr        , 10 );
            if ( cfs_period_ptr == value_pair ) // "max": no quota in place
                cfs_quota = -1;
        }
        else // cgroup v1
        {
            cfs_quota  = read_int( "/sys/fs/cgroup/cpu/cpu.cfs_quota_us"  );
            cfs_period = read_int( "/sys/fs/cgroup/cpu/cpu.cfs_period_us" );
        }

        if ( ( cfs_quota > 0 ) && ( cfs_period > 0 ) )
        {
            // Docker allows non-whole core quota assignments - use some sort of
            // heurestical rounding.
            return std::max( static_cast<int>( ( cfs_quota + cfs_period / 2 ) / cfs_period ), 1 );
        }
        return -1;
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
