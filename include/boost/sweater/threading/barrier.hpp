////////////////////////////////////////////////////////////////////////////////
///
/// \file barrier.hpp
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
#if defined( __APPLE__ )
// TODO
// https://developer.apple.com/forums/thread/707288
// https://lists.apple.com/archives/darwin-dev/2018/Jul/msg00003.html
// https://webkit.org/blog/6161/locking-in-webkit
#include "generic_barrier.hpp"
#else
#include "futex_barrier.hpp"
#endif
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

#if defined( __APPLE__ )
using barrier = generic_barrier;
#else
using barrier = futex_barrier;
#endif

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
