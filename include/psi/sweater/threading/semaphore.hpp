////////////////////////////////////////////////////////////////////////////////
///
/// \file semaphore.hpp
/// -------------------
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
#include "futex.hpp"
#include "hardware_concurrency.hpp"

#include <atomic>
#include <cstdint>
#include <type_traits>

#ifdef __APPLE__ // condvar, not futex: measured ~1.6x faster for the
// semaphore's signal-heavy fire path on Apple Silicon (sweater_shop_bench),
// unlike the barrier's join pattern which does profit from the futex.
// Root-caused with per-impl park/wake counters. It is NOT a
// no-waiter-fast-path difference (both impls skip the wake syscall when no
// waiter is registered) and NOT __ulock_wake's lack of an exact wake count
// (a wake_one-loop variant measured the same as ULF_WAKE_ALL). The futex
// protocol's waiters_ gate merely OVERCOUNTS sleepers: a woken worker stays
// registered across its whole wake -> retry -> (lose the token race) ->
// re-park window, so under the bench's fire pattern ~92% of signals paid a
// wake syscall — mostly aimed at workers that were already awake — and the
// token stealing added ~40% more park syscalls. The condvar impl's
// mutex-serialized waiters_/to_release_ bookkeeping is an exact "is anyone
// actually asleep" test plus a credit that guarantees every wake is consumed
// (no steal/re-park churn): only ~23% of its signals reached
// pthread_cond_signal. A futex protocol with condvar-style exact sleeper
// accounting could plausibly close the gap — unexplored, as the condvar
// already delivers, and keeping the semaphore private-API-free on all of
// Apple is worth having anyway (the embedded OSes have no futex backend at
// all — see futex.hpp).
#include "condvar.hpp"
#include "mutex.hpp"

#include <mutex>
#endif // Apple
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

class semaphore
{
public:
    semaphore() noexcept = default;
#ifndef NDEBUG
   ~semaphore() noexcept;
#endif // !NDEBUG

    void signal( hardware_concurrency_t count = 1 ) noexcept;

    void wait(                          ) noexcept;
    void wait( std::uint32_t spin_count ) noexcept;

#if !defined( __APPLE__ ) // see the condvar include note above ///////////////

private:
    using signed_futex_value_t =  std::make_signed_t< futex::value_type >;
    enum state : signed_futex_value_t { locked = 0, contested = -1 };

    signed_futex_value_t load( std::memory_order ) const noexcept;

    bool try_decrement( signed_futex_value_t & last_value ) noexcept;

private:
    futex                               value_   = { state::locked };
    std::atomic<hardware_concurrency_t> waiters_ = 0                ;

#else // condvar impl for Apple (see above) ///////////////////////////////////

private:
    std::atomic<std::int32_t> value_      = 0; // atomic to support spin-waits
    hardware_concurrency_t    waiters_    = 0; // to enable detection when notify_all() can be used
    hardware_concurrency_t    to_release_ = 0;
    mutex                     mutex_    ;
    condition_variable        condition_;

#endif // Apple ///////////////////////////////////////////////////////////////


}; // class semaphore

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
