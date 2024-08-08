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
#include <boost/config.hpp>
// TODO https://developer.apple.com/documentation/os/1646466-os_unfair_lock_lock
#if defined( BOOST_HAS_PTHREADS )
#include "posix/mutex.hpp"
#else
#include "windows/mutex.hpp"
#endif
//------------------------------------------------------------------------------
namespace boost::thrd_lite
{
//------------------------------------------------------------------------------

#if defined( BOOST_HAS_PTHREADS )
using mutex = pthread_mutex;
#else
using mutex = win32_slim_mutex;
#endif

//------------------------------------------------------------------------------
} // namespace boost::thrd_lite
//------------------------------------------------------------------------------
