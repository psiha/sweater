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

// Backend selection. The futex impl now uses the same exact sleeper
// accounting (credits consumable only by the sleep path, gated by a precise
// sleeper count) that made the condvar impl win the original Apple A/B --
// see futex_semaphore.cpp's design-doc comment for the protocol and
// semaphore-history: the ORIGINAL futex protocol's waiters_ gate overcounted
// sleepers (a woken worker stayed registered across its whole wake -> retry
// -> lose-the-token-race -> re-park window), paying a wake syscall on ~92% of
// the bench's fire signals (vs ~23% of signals reaching pthread_cond_signal
// under the condvar impl) plus ~40% extra parks from token stealing. That
// verdict was protocol accounting, NOT primitive cost: it was neither a
// no-waiter-fast-path difference (both impls skipped the syscall with no
// waiter registered) nor __ulock_wake's lack of an exact wake count (a
// wake_one-loop variant measured the same as ULF_WAKE_ALL).
// Apple stays condvar-backed by default regardless: the embedded OSes have no
// futex backend at all (see futex.hpp) and macOS ships the private-API-free
// variant unless a measured win justifies opting in
// (PSI_SWEATER_FUTEX_SEMAPHORE, the sweater.cmake A/B knob).
#if defined( __APPLE__ ) && !defined( PSI_SWEATER_FUTEX_SEMAPHORE )
#define PSI_SWEATER_CONDVAR_SEMAPHORE 1
#include "condvar.hpp"
#include "mutex.hpp"

#include <mutex>
#else
#define PSI_SWEATER_CONDVAR_SEMAPHORE 0
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

#if !PSI_SWEATER_CONDVAR_SEMAPHORE // futex impl //////////////////////////////

private:
    using signed_futex_value_t = std::make_signed_t< futex::value_type >;

    bool try_decrement( signed_futex_value_t & last_value ) noexcept;

private:
    // Exact sleeper accounting (see futex_semaphore.cpp's design-doc comment):
    // tokens and wake credits are SEPARATE words -- spinners/arrivals contend
    // only on value_, parked workers are woken through credits_ which only the
    // sleep path consumes.
    std::atomic<signed_futex_value_t>   value_    = 0    ; // token count; negative = sleepers in debt
    futex                               credits_  = { 0 }; // wake credits, consumed only by the sleep path
    std::atomic<hardware_concurrency_t> sleepers_ = 0    ; // exact count of workers parked (or committed to park) on credits_

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
