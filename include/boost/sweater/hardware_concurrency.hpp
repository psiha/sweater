////////////////////////////////////////////////////////////////////////////////
///
/// \file hardware_concurrency.hpp
/// ------------------------------
///
/// (c) Copyright Domagoj Saric 2016 - 2019.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#ifndef hardware_concurrency_hpp__070F2563_7F3C_40D5_85C9_1289E8DEEDC8
#define hardware_concurrency_hpp__070F2563_7F3C_40D5_85C9_1289E8DEEDC8
#pragma once
//------------------------------------------------------------------------------
#ifndef BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
#   if defined( __EMSCRIPTEN__ ) && !defined( __EMSCRIPTEN_PTHREADS__ )
#       define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 1
#   elif defined( __ANDROID__ )
#       if defined( __aarch64__ )
#           define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 32 // SGS6 8, Meizu PRO 6 10 cores
#       elif defined( __arm__ )
#           define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 8
#       endif // arch
#   elif defined( __APPLE__ )
#       if defined( __aarch64__ )
#           define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 8 // iPad 2 Air 3, iPhone 8 6 cores
#       elif defined( __arm__ )
#           define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 2
#       else
#          define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 0 // desktop or simulator
#       endif // arch
#   elif defined(__WINDOWS_PHONE__)
#	    define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 2
#   else
#	    define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 0
#   endif // platform
#endif // BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY

#ifdef __ANDROID__
#    include <unistd.h>
#endif // __ANDROID__
#ifdef __EMSCRIPTEN_PTHREADS__
#   include <emscripten/threading.h>
#endif // __EMSCRIPTEN_PTHREADS__
#ifdef __linux__
#   include <sys/sysinfo.h>
#endif // __linux__

#if BOOST_SWEATER_DOCKER_LIMITS
#   include <boost/assert.hpp>

#   include <fcntl.h>
#   include <sys/types.h>
#   include <unistd.h>
#endif

#ifdef _MSC_VER
#   include <yvals.h>
#   pragma detect_mismatch( "BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY", _STRINGIZE( BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY ) )
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
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------

#if defined( __arm__ ) || defined( __aarch64__ ) || defined( __ANDROID__ ) || defined( __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ )
using hardware_concurrency_t = std::uint_fast8_t;
#else
using hardware_concurrency_t = std::uint_fast16_t; // e.g. Intel MIC
#endif

#if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY == 1

inline hardware_concurrency_t       hardware_concurrency_current() noexcept { return 1; }
inline hardware_concurrency_t const hardware_concurrency_max{ 1 };

#elif BOOST_SWEATER_DOCKER_LIMITS

namespace detail
{
    inline int read_int( char const * const file_path ) noexcept
    {
        auto const fd( ::open( file_path, O_RDONLY, 0 ) );
        if ( fd == -1 )
            return -1;
        char value[ 64 ];
        BOOST_VERIFY( ::read( fd, value, sizeof( value ) ) < signed( sizeof( value ) ) );
        return std::atoi( value );
    }

    inline auto const get_docker_limit() noexcept
    {
        // https://bugs.openjdk.java.net/browse/JDK-8146115
        // http://hg.openjdk.java.net/jdk/hs/rev/7f22774a5f42
        // RAM limit /sys/fs/cgroup/memory.limit_in_bytes
        // swap limt /sys/fs/cgroup/memory.memsw.limit_in_bytes

        auto const cfs_quota ( read_int( "/sys/fs/cgroup/cpu/cpu.cfs_quota_us"  ) );
        auto const cfs_period( read_int( "/sys/fs/cgroup/cpu/cpu.cfs_period_us" ) );
        if ( ( cfs_quota > 0 ) && ( cfs_period > 0 ) )
        {
            // Docker allows non-whole core quota assignments - use some sort of
            // heurestical rounding.
            return std::max( ( cfs_quota + cfs_period / 2 ) / cfs_period, 1 );
        }
        return -1;
    }
} // namespace detail

inline struct hardware_concurrency_max_t
{
    int const docker_quota = detail::get_docker_limit();

    hardware_concurrency_t const value = static_cast<hardware_concurrency_t>( ( docker_quota != -1 ) ? docker_quota : get_nprocs_conf() );

    operator hardware_concurrency_t() const noexcept { return value; }
} const hardware_concurrency_max __attribute__(( init_priority( 101 ) ));

inline auto hardware_concurrency_current() noexcept { return static_cast<hardware_concurrency_t>( ( hardware_concurrency_max.docker_quota != -1 ) ? hardware_concurrency_max.docker_quota : get_nprocs() ); }

#else // generic/standard impl

namespace detail
{
    inline auto get_hardware_concurrency_max() noexcept
    {
        return static_cast<hardware_concurrency_t>
        (
#       if defined( __EMSCRIPTEN_PTHREADS__ )
#         if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY == 0
            emscripten_has_threading_support() ? emscripten_num_logical_cores() : 1
#         else
            BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
#         endif
#       elif defined( __linux__ )
            // libcpp std::thread::hardware_concurrency() returns the dynamic number of active cores.
            get_nprocs_conf()
#       else
            std::thread::hardware_concurrency()
#       endif
        );
    }
} // namespace detail

inline auto hardware_concurrency_current() noexcept
{
    return static_cast<hardware_concurrency_t>
    (
#   if defined( __EMSCRIPTEN_PTHREADS__ )
        detail::get_hardware_concurrency_max()
#   elif defined( __linux__ )
        get_nprocs()
#   else
        std::thread::hardware_concurrency()
#   endif
    );
}

#ifdef __GNUC__
// http://clang-developers.42468.n3.nabble.com/Clang-equivalent-to-attribute-init-priority-td4034229.html
// https://gcc.gnu.org/ml/gcc-help/2011-05/msg00221.html
// "can only use 'init_priority' attribute on file-scope definitions of objects of class type"
inline struct hardware_concurrency_max_t
{
    hardware_concurrency_t const value = detail::get_hardware_concurrency_max();
    operator hardware_concurrency_t() const noexcept { return value; }
} const hardware_concurrency_max __attribute__(( init_priority( 101 ) ));
#else
inline auto const hardware_concurrency_max( detail::get_hardware_concurrency_max() );
#endif // compiler

#endif // impl

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // hardware_concurrency_hpp
