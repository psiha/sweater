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
#   if defined( __ANDROID__ )
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
#ifdef __linux__
#   include <sys/sysinfo.h>
#endif // __linux__

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

namespace detail
{
    inline auto get_hardware_concurrency_max() noexcept
    {
        return static_cast<hardware_concurrency_t>
        (
#       ifdef __linux__
            // libcpp std::thread::hardware_concurrency() returns the dynamic number of active cores.
            get_nprocs_conf()
#       else
            std::thread::hardware_concurrency()
#       endif
        );
    }
} // namespace detail

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

inline auto hardware_concurrency_current() noexcept
{
    return static_cast<hardware_concurrency_t>
    (
#   ifdef __linux__
        get_nprocs()
#   else
        std::thread::hardware_concurrency()
#   endif
    );
}

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // hardware_concurrency_hpp
