////////////////////////////////////////////////////////////////////////////////
///
/// \file condvar.hpp
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
#pragma once
//------------------------------------------------------------------------------
#if !defined( _WIN32 )
#include "posix/condvar.hpp"
#else
#include "windows/condvar.hpp"
#endif
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

#if !defined( _WIN32 )
using condition_variable = pthread_condition_variable;
#else
using condition_variable = win32_condition_variable;
#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
