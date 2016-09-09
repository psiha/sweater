////////////////////////////////////////////////////////////////////////////////
///
/// \file sweater.hpp
/// -----------------
///
/// (c) Copyright Domagoj Saric 2016.
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
#if defined( __ANDROID__ )
#	include "detail/android.hpp"
#elif defined( __APPLE__ )
#	include "detail/apple.hpp"
#elif defined( _WIN32 )
#	include "detail/windows.hpp"
#elif defined( _OPENMP )
#	include "detail/openmp.hpp"
#else
#	include "detail/generic.hpp"
#endif
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------

class shop
{
public:
}; // class shop

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // sweater_hpp
