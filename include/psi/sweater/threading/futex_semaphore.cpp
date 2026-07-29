////////////////////////////////////////////////////////////////////////////////
///
/// \file futex_semaphore.cpp
/// -------------------------
///
/// (c) Copyright Domagoj Saric 2016 - 2026.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "semaphore.hpp"

#if !PSI_SWEATER_CONDVAR_SEMAPHORE

#include "cpp/spin_lock.hpp" // only for nops()

#include <boost/assert.hpp>

#include <algorithm>
#ifdef PSI_SWEATER_SEMA_STATS
#include <cinttypes>
#include <cstdio>
#endif
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Exact-sleeper-accounting futex semaphore
// ---------------------------------------------------------------------------
// A lock-free transcription of the bookkeeping that made the condvar impl
// (generic_semaphore.cpp) win the original Apple futex-vs-condvar A/B: the
// previous protocol here (single futex word carrying the token count with a
// `contested` marker, gated by an approximate waiters_ count) paid a wake
// syscall on ~92% of the fire-path signals -- mostly aimed at workers that
// were already awake, because a woken worker stayed registered in waiters_
// across its whole wake -> retry -> (lose the token race to a spinner) ->
// re-park window -- plus ~40% extra park syscalls from that token stealing.
// The condvar impl only reached pthread_cond_signal on ~23% of signals: its
// mutex-serialized bookkeeping wakes only actual sleepers and hands each one
// a credit (to_release_) that a spinner cannot steal. This protocol keeps
// those two properties without the mutex:
//
//   value_    tokens (signed; negative = that many waiters in debt). The ONLY
//             word spinners and the signal fast path touch.
//   credits_  wake credits, a separate futex word consumed EXCLUSIVELY by the
//             sleep path -- the lock-free to_release_. A woken sleeper takes
//             a credit instead of re-racing spinners for value_, so a wake
//             cannot be "stolen" by a thread that was never asleep.
//   sleepers_ exact count of threads parked (or committed to parking) on
//             credits_. Registration lasts only from just-before-parking to
//             credit consumption -- NOT across the old protocol's whole
//             retry window -- so the signal path's "is anyone actually
//             asleep" gate is as tight as the condvar's.
//
// signal: fetch_add tokens; a non-negative previous value means nobody is in
//   debt -> done, no further shared-state access at all (the condvar impl's
//   old_value > 0 early-out, extended: it also skips the mutex round-trip).
//   Otherwise deposit min(debt, count) credits and wake that many parked
//   sleepers -- iff the exact gate says any exist.
// wait: fetch_sub a token; a positive previous value is the uncontended fast
//   path. Otherwise the thread owes a sleep: register in sleepers_, then loop
//   "consume a credit or park on credits_ == 0".
//
// Missed-wakeup safety (the Dekker/store-buffer pattern): the signal side
// deposits into credits_ (a seq_cst RMW) then loads sleepers_ (seq_cst); the
// wait side registers in sleepers_ (a plain acquire RMW -- the shared
// overflow_checked_inc helper) then issues a seq_cst FENCE before its
// credits_ loads. The fence participates in the same single total order S as
// the signal side's seq_cst operations, which restores the pairing the
// all-seq_cst formulation would give ([atomics.order]): either the signal
// side's deposit precedes the fence in S -- then the sleeper's credits_ load
// (sequenced after the fence) observes the deposited credit and consumes it
// without parking -- or the fence precedes the deposit, in which case the
// signal side's sleepers_ load (sequenced after its deposit, later still in
// S) observes the registration (sequenced before the fence) and issues the
// wake. Either way at least one side sees the other; a signaler may skip the
// syscall only when no registered sleeper can end up parked against the
// pre-deposit credits_ value. The futex's atomic value re-check on park
// covers the remaining in-between: a deposit landing between the sleeper's
// load and its park makes wait_if_equal( 0 ) return immediately.
//
// A woken sleeper can still find credits_ == 0 -- another REGISTERED sleeper
// (about to park, never parked) may consume the credit first and skip its own
// park; the woken one re-parks. That churn is bounded by the sleeper count
// and self-compensating (a park was skipped elsewhere), unlike the old
// protocol where any spinner could take the token and force the woken worker
// straight back to sleep.
// ---------------------------------------------------------------------------

// Here we only use global semaphore objects so there is no need for the
// race-condition workaround described in the links below.
// http://git.musl-libc.org/cgit/musl/commit/?id=88c4e720317845a8e01aee03f142ba82674cd23d
// https://github.com/preshing/cpp11-on-multicore/blob/master/common/sema.h
// https://stackoverflow.com/questions/36094115/c-low-level-semaphore-implementation
// https://comp.programming.threads.narkive.com/IRKGW6HP/too-much-overhead-from-semaphores

#ifdef PSI_SWEATER_SEMA_STATS // A/B instrumentation: per-process signal/wake/park tallies, printed at exit
namespace
{
    std::atomic<std::uint64_t> sema_signals{ 0 }, sema_wake_syscalls{ 0 }, sema_parks{ 0 };
    struct sema_stats_printer
    {
        ~sema_stats_printer() { std::fprintf( stderr, "[SEMA futex-credit] signals=%" PRIu64 " wake_syscalls=%" PRIu64 " parks=%" PRIu64 "\n", sema_signals.load(), sema_wake_syscalls.load(), sema_parks.load() ); }
    } const sema_stats_printer_instance;
} // anonymous namespace
#define PSI_SEMA_COUNT( which ) which.fetch_add( 1, std::memory_order_relaxed )
#else
#define PSI_SEMA_COUNT( which )
#endif // PSI_SWEATER_SEMA_STATS

#ifndef NDEBUG
semaphore::~semaphore() noexcept
{
#if 0 // need not hold on early destruction (when workers exit before waiting)
    BOOST_ASSUME( value_   == 0 );
#endif
#if !defined( _MSC_VER ) // TODO investigate: known to fail on process termination/global shop destruction
    BOOST_ASSERT( sleepers_ == 0 );
#endif
}
#endif // !NDEBUG

void semaphore::signal( hardware_concurrency_t const count /*= 1*/ ) noexcept
{
    // https://softwareengineering.stackexchange.com/questions/340284/mutex-vs-semaphore-how-to-implement-them-not-in-terms-of-the-other

#if PSI_SWEATER_EXACT_WORKER_SELECTION && !defined( __ANDROID__ )
    BOOST_ASSUME( count == 1 );
#endif // PSI_SWEATER_EXACT_WORKER_SELECTION
    if ( PSI_UNLIKELY( !count ) )
        return;

    PSI_SEMA_COUNT( sema_signals );
    auto const old_value{ value_.fetch_add( static_cast<signed_futex_value_t>( count ), std::memory_order_release ) };
    if ( old_value >= 0 )
        return; // nobody in debt -- no sleeper can exist for these tokens

    // Deposit BEFORE reading sleepers_ (the seq_cst store->load half of the
    // Dekker pairing documented above).
    auto const to_wake{ std::min( static_cast<hardware_concurrency_t>( -old_value ), count ) };
    credits_.fetch_add( to_wake, std::memory_order_seq_cst );
    if ( sleepers_.load( std::memory_order_seq_cst ) )
    {
        PSI_SEMA_COUNT( sema_wake_syscalls );
        credits_.wake( to_wake );
    }
}

void semaphore::wait() noexcept
{
    auto const old_value{ value_.fetch_sub( 1, std::memory_order_acquire ) };
    if ( old_value > 0 )
        return;

    // In debt: this thread owes a sleep and is entitled to exactly one credit.
    // Register, then a seq_cst fence, then load credits -- the wait side's
    // half of the Dekker pairing, fence-based so the registration itself can
    // stay the shared overflow_checked_inc helper (see the design-doc comment
    // above for why the fence gives the same guarantee as an all-seq_cst
    // formulation).
    detail::overflow_checked_inc( sleepers_ );
    std::atomic_thread_fence( std::memory_order_seq_cst );
    for ( ; ; )
    {
        auto credits{ credits_.load( std::memory_order_acquire ) };
        while ( credits > 0 )
        {
            if ( credits_.compare_exchange_weak( credits, credits - 1, std::memory_order_acquire, std::memory_order_relaxed ) )
            {
                detail::underflow_checked_dec( sleepers_ );
                return;
            }
        }
        PSI_SEMA_COUNT( sema_parks );
        credits_.wait_if_equal( 0 );
    }
}

void semaphore::wait( std::uint32_t const spin_count ) noexcept
{
    // Spin only on the token word -- never on credits_, which belongs to
    // registered sleepers alone (what makes wakes steal-proof; see above).
    auto value{ static_cast<signed_futex_value_t>( value_.load( std::memory_order_acquire ) ) };
    for ( auto spin_try{ 0U }; spin_try < spin_count; )
    {
        if ( value > 0 )
        {
            if ( PSI_LIKELY( try_decrement( value ) ) )
                return;
        }
        else
        {
            nops( 8 );
            value = value_.load( std::memory_order_acquire );
            ++spin_try;
        }
    }
    // value could be > 0 here (in case the change happened on the last try)

    wait();
}

bool semaphore::try_decrement( signed_futex_value_t & __restrict last_value ) noexcept
{
    return PSI_LIKELY
    (
        value_.compare_exchange_weak
        (
            last_value,
            static_cast<signed_futex_value_t>( last_value - 1 ),
            std::memory_order_acquire,
            std::memory_order_relaxed
        )
    );
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
#endif // !PSI_SWEATER_CONDVAR_SEMAPHORE
