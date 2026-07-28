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
// Futex-backed wherever psi::thrd_lite::futex has a backend — macOS included,
// through the __ulock futex backend (apple/futex.cpp; see the private-API
// caveat there — swapping that single backend to the public
// os_sync_wait_on_address API lifts it for the whole thrd_lite collection at
// once, and would also unlock the embedded Apple platforms). Apple's embedded
// OSes (iOS & co.) cannot ship private syscalls (App Store review) so they
// have no futex backend at all (see futex.hpp) and keep the condvar-based
// generic_barrier.
#include "futex.hpp" // PSI_THRD_LITE_HAS_FUTEX
#if PSI_THRD_LITE_HAS_FUTEX
#include "futex_barrier.hpp"
#else
#include "generic_barrier.hpp"
#endif
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

#if PSI_THRD_LITE_HAS_FUTEX
using barrier = futex_barrier;
#else
using barrier = generic_barrier;
#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
