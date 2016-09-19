////////////////////////////////////////////////////////////////////////////////
///
/// \file windows.hpp
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
#pragma once
//------------------------------------------------------------------------------
#ifndef BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
#if defined(__WINDOWS_PHONE__)
#	define BOOST_SWEATER_MAX_HARDWARE_CONCURENCY 2;
#else
#	define BOOST_SWEATER_MAX_HARDWARE_CONCURENCY 0;
#endif
#endif // BOOST_SWEATER_MAX_HARDWARE_CONCURENCY

#include "generic.hpp"
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------