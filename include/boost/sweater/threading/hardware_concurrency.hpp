////////////////////////////////////////////////////////////////////////////////
///
/// \file hardware_concurrency.hpp
/// ------------------------------
///
/// (c) Copyright Domagoj Saric 2016 - 2021.
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

#include <cstdint>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

#if defined( __arm__ ) || defined( __aarch64__ ) || defined( __ANDROID__ ) || defined( __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ )
using hardware_concurrency_t = std::uint8_t;
#else
using hardware_concurrency_t = std::uint16_t; // e.g. Intel MIC
#endif

hardware_concurrency_t hardware_concurrency_current() noexcept;
hardware_concurrency_t get_hardware_concurrency_max() noexcept;

extern struct hardware_concurrency_max_t
{
    hardware_concurrency_t const value{ get_hardware_concurrency_max() };
    operator hardware_concurrency_t() const noexcept { return value; }
} const hardware_concurrency_max;


#ifdef __ANDROID__
extern struct slow_thread_signals_t
{
    slow_thread_signals_t() noexcept;
    bool const value;
    __attribute__(( const )) operator bool() const noexcept { return value; }
} const slow_thread_signals;
#else
bool constexpr slow_thread_signals{ false };
#endif // ANDROID

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // hardware_concurrency_hpp
