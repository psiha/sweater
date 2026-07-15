////////////////////////////////////////////////////////////////////////////////
///
/// \file futex.hpp
/// ---------------
///
/// (c) Copyright Domagoj Saric 2021.
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
#include "hardware_concurrency.hpp"

#include <atomic>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// https://www.remlab.net/op/futex-misc.shtml
// https://dept-info.labri.fr/~denis/Enseignement/2008-IR/Articles/01-futex.pdf

struct futex : std::atomic
<
#ifdef _WIN32
    hardware_concurrency_t
#else
    std::uint32_t
#endif
>
{
    // Match-any bitset for the bitset-aware overloads below: on backends that support
    // per-waiter-category filtering (Linux FUTEX_WAIT_BITSET/FUTEX_WAKE_BITSET), this
    // value is FUTEX_BITSET_MATCH_ANY (all bits set) -- i.e. "no filtering", identical
    // to the plain (non-bitset) wait/wake semantics. Backends without a bitset concept
    // (Windows WaitOnAddress/WakeByAddress*, Apple __ulock_wait/__ulock_wake, Emscripten)
    // simply ignore the argument -- callers on those backends still compile and behave
    // exactly as before this API was added; the parameter is a Linux-only optimization
    // hook, not a portability requirement.
    static constexpr value_type all_bits = static_cast<value_type>( -1 );

    void wake_one(                                        ) const noexcept;
    void wake    ( hardware_concurrency_t waiters_to_wake ) const noexcept;
    // wake_bitset: like wake_all(), but on backends that support it, only wakes waiters
    // parked via wait_if_equal(value, listen_bits) where (listen_bits & wake_bitset) != 0
    // -- e.g. distinguishing "wake a parked writer" from "wake a parked reader" sharing
    // the same futex word, without waking (and needlessly re-parking) the other
    // category. Defaults to all_bits, i.e. behaviorally identical to wake_all().
    void wake_all( value_type wake_bitset = all_bits ) const noexcept;

    // listen_bits: which wake_bitset values this parked wait should respond to (see
    // wake_all above). Defaults to all_bits, i.e. behaviorally identical to a plain wait.
    void wait_if_equal( value_type desired_value, value_type listen_bits = all_bits ) const noexcept;
}; // struct futex

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
