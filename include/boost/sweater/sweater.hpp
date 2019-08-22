////////////////////////////////////////////////////////////////////////////////
///
/// \file sweater.hpp
/// -----------------
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
#ifndef sweater_hpp__83B147A4_8450_4A6D_8FC1_72EA64FACABF
#define sweater_hpp__83B147A4_8450_4A6D_8FC1_72EA64FACABF
#pragma once
//------------------------------------------------------------------------------
#include "hardware_concurrency.hpp"

#  if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY == 1
#   define BOOST_SWEATER_IMPL single_threaded
#	include "detail/single_threaded.hpp"
#elif defined( __APPLE__ ) && !defined( BOOST_SWEATER_IMPL )
#   define BOOST_SWEATER_IMPL apple
#	include "detail/apple.hpp"
#elif defined( _WIN32_unimplemented ) && !defined( BOOST_SWEATER_IMPL )
#   define BOOST_SWEATER_IMPL windows
#	include "detail/windows.hpp"
#elif defined( _OPENMP ) && !defined( BOOST_SWEATER_IMPL )
#   define BOOST_SWEATER_IMPL openmp
#	include "detail/openmp.hpp"
#elif defined( BOOST_SWEATER_IMPL )
#include "boost/preprocessor/stringize.hpp"
#	include BOOST_PP_STRINGIZE( detail/BOOST_SWEATER_IMPL.hpp )
#else
#   undef  BOOST_SWEATER_IMPL
#   define BOOST_SWEATER_IMPL generic
#	include "detail/generic.hpp"
#endif

#ifdef _MSC_VER
#   include <yvals.h>
#   pragma detect_mismatch( "Boost.Sweater implementation", _STRINGIZE( BOOST_SWEATER_IMPL ) )
#endif // _MSC_VER
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------

using namespace BOOST_SWEATER_IMPL;

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // sweater_hpp
