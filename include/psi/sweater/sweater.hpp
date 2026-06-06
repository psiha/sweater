////////////////////////////////////////////////////////////////////////////////
///
/// \file sweater.hpp
/// -----------------
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
#ifndef sweater_hpp__83B147A4_8450_4A6D_8FC1_72EA64FACABF
#define sweater_hpp__83B147A4_8450_4A6D_8FC1_72EA64FACABF
#pragma once
//------------------------------------------------------------------------------
#include "threading/hardware_concurrency.hpp"

#  if PSI_SWEATER_MAX_HARDWARE_CONCURRENCY == 1
#   define PSI_SWEATER_IMPL single_threaded
#	include "impls/single_threaded.hpp"
#elif defined( __APPLE__ ) && !defined( PSI_SWEATER_IMPL )
#   define PSI_SWEATER_IMPL apple
#	include "impls/apple.hpp"
#elif defined( _WIN32 ) && !defined( PSI_SWEATER_IMPL )
#   define PSI_SWEATER_IMPL windows
#	include "impls/windows.hpp"
#elif defined( _OPENMP ) && !defined( PSI_SWEATER_IMPL )
#   define PSI_SWEATER_IMPL openmp
#	include "impls/openmp.hpp"
#elif defined( PSI_SWEATER_IMPL )
#include <boost/preprocessor/stringize.hpp>
#	include BOOST_PP_STRINGIZE( detail/PSI_SWEATER_IMPL.hpp )
#else
#   undef  PSI_SWEATER_IMPL
#   define PSI_SWEATER_IMPL generic
#	include "impls/generic.hpp"
#endif

// Detect linker-level ODR mismatches between TUs compiled with different impls.
// Skipped on clang-cl: macro-stringize does not expand to a string literal
// inside #pragma detect_mismatch arguments there.
#if defined( _MSC_VER ) && !defined( __clang__ )
#   include <yvals.h>
#   pragma detect_mismatch( "psi::sweater implementation", _STRINGIZE( PSI_SWEATER_IMPL ) )
#endif // MSVC (not clang-cl)

//------------------------------------------------------------------------------
namespace psi::sweater
{
//------------------------------------------------------------------------------

using namespace PSI_SWEATER_IMPL;

//------------------------------------------------------------------------------
} // namespace psi::sweater
//------------------------------------------------------------------------------
#endif // sweater_hpp
