////////////////////////////////////////////////////////////////////////////////
///
/// \file mutex.hpp
/// ---------------
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
// TODO https://developer.apple.com/documentation/os/1646466-os_unfair_lock_lock
#if !defined( _WIN32 )
#include "posix/mutex.hpp"
#else
#include "windows/mutex.hpp"
#endif
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

#if !defined( _WIN32 )
using mutex = pthread_mutex;
#else
using mutex = win32_slim_mutex;
#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
