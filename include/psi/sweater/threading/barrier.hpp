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
// Futex-backed on every platform — Apple included, through the __ulock futex
// backend (apple/futex.cpp; see the private-API caveat there — swapping that
// single backend to the public os_sync_wait_on_address API lifts it for the
// whole thrd_lite collection at once).
#include "futex_barrier.hpp"
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

using barrier = futex_barrier;

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
