////////////////////////////////////////////////////////////////////////////////
///
/// \file hardware_concurrency.hpp
/// ------------------------------
///
/// (c) Copyright Domagoj Saric 2016 - 2017.
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

#ifdef _MSC_VER
#   include <yvals.h>
#   pragma detect_mismatch( "BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY", _STRINGIZE( BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY ) )
#endif // _MSC_VER

#include <boost/config_ex.hpp>

#include <cstdint>
#include <thread>
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

#ifdef BOOST_MSVC // no inline variables even in VS 15.3
BOOST_OVERRIDABLE_SYMBOL
#else
inline
#endif // BOOST_MSVC
auto const hardware_concurrency( static_cast<hardware_concurrency_t>( std::thread::hardware_concurrency() ) );

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // hardware_concurrency_hpp
