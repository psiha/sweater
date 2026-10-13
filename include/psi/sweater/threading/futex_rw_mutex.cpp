////////////////////////////////////////////////////////////////////////////////
///
/// \file futex_rw_mutex.cpp
/// ------------------------
///
/// (c) Copyright Domagoj Saric 2026.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "futex_rw_mutex.hpp"

#if PSI_THRD_LITE_HAS_FUTEX // no futex backend on Apple's embedded OSes (see futex.hpp)

#if defined( _MSC_VER ) && !defined( __clang__ )
#include <intrin.h>
#endif
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

namespace
{
    // Spin-wait hint, self-contained on purpose: this TU is also built by consumers
    // that compile it without the rest of the library.
    void cpu_relax() noexcept
    {
#if defined( __x86_64__ ) || defined( __i386__ )
        __builtin_ia32_pause();
#elif defined( __aarch64__ ) || defined( __arm__ )
        asm volatile( "yield" ::: "memory" );
#elif defined( _M_X64 ) || defined( _M_IX86 )
        _mm_pause();
#elif defined( _M_ARM64 ) || defined( _M_ARM )
        __yield();
#else
        std::atomic_signal_fence( std::memory_order_seq_cst );
#endif
    }
} // anonymous namespace

// Out-of-line on purpose (hot-path inlining across the header boundary is
// LTO's job): the header keeps only the trivial wrappers and the design doc.

#ifdef PSI_THRD_LITE_FUTEX_RW_MUTEX_COUNTERS // test-only hook, see the header
futex_rw_mutex_counters futex_rw_mutex_call_counters;
#define PSI_FUTEX_RW_MUTEX_COUNT( counter ) futex_rw_mutex_call_counters.counter.fetch_add( 1, std::memory_order_relaxed )
#else
#define PSI_FUTEX_RW_MUTEX_COUNT( counter ) ( void )0
#endif

void futex_rw_mutex::release_ro_core() noexcept
{
    auto const old{ state_.fetch_sub( 1, std::memory_order_release ) };
    BOOST_ASSERT_MSG( ( old & reader_mask ) != 0, "release_ro without a matching acquire_ro" );
    // Wake only if a writer announced it is about to sleep (see the header's
    // design-doc comment): the reader count passes through zero constantly under
    // reader preference and a wake syscall per zero crossing is a wake storm on
    // backends that wake every waiter (Apple) -- one kernel bucket, all cores.
    if ( ( old & ( reader_mask | writer_parked_bit ) ) == ( 1 | writer_parked_bit ) )
    {
        // Claim the announcement atomically: only the release that actually clears
        // the bit wakes, so there is at most one wake per announcement. A reader
        // that entered since the fetch_sub only makes the woken writer re-announce.
        if ( state_.fetch_and( state_t( ~writer_parked_bit ), std::memory_order_release ) & writer_parked_bit )
        {
            PSI_FUTEX_RW_MUTEX_COUNT( release_ro_wakes );
            // Bitset-targeted at writer_wait_bits where the backend has a real bitset
            // (Linux); elsewhere every parked thread is woken, which is why this must
            // be a wake_all and never a wake_one: the announcement is shared by all
            // parked writers and a single wake could reach a reader instead.
            state_.wake_all( writer_wait_bits );
        }
    }
}

void futex_rw_mutex::release_ro() noexcept
{
    detail::on_ro_release( this );
    release_ro_core();
}

void futex_rw_mutex::acquire_rw() noexcept
{
    static constexpr unsigned spin_rounds{ 64 }; // bounded look-again budget before each sleep (see below)

    auto observed{ state_.load( std::memory_order_relaxed ) };
    auto spins_left{ spin_rounds };
    for ( ; ; )
    {
        // "Free to take" means no readers AND no writer currently holding --
        // announce bits may already be set (by us, on a previous iteration of
        // this very loop, or by another queued writer) and must NOT block us:
        // requiring the whole word to be exactly 0 here is the bug this replaced --
        // a writer that had announced itself could never again observe state 0
        // once readers drained (the bits it set itself permanently excluded the
        // very state it was waiting for -- a self-inflicted missed wakeup).
        if ( ( observed & ( reader_mask | writer_locked_bit ) ) == 0 )
        {
            // PRESERVE the parked bits (they may cover OTHER threads still parked
            // on them): with release_rw's wake conditional on them, clearing either
            // here would strand those threads. They are cleared by release_rw's
            // exchange( 0 ) or by the release_ro that wakes the writers. (A writer
            // that announced and then won without sleeping thus carries its own
            // stale bit through the hold -- costing release one possibly-redundant
            // wake.) writer_waiting_bit is NOT carried over: it only gates new
            // readers, which writer_locked_bit does for the whole hold.
            if ( state_.compare_exchange_weak( observed, state_t( writer_locked_bit | ( observed & parked_bits ) ), std::memory_order_acquire, std::memory_order_relaxed ) )
            {
                return;
            }
            continue; // observed refreshed by the failed CAS; retry
        }
        if ( ( observed & writer_waiting_bit ) == 0 )
        {
            // Want the lock: from the first failed attempt on, the writer-preferring
            // lock admits no new readers. Lost CAS = the word changed, re-evaluate.
            auto const desired{ state_t( observed | writer_waiting_bit ) };
            if ( state_.compare_exchange_weak( observed, desired, std::memory_order_relaxed, std::memory_order_relaxed ) )
            {
                observed = desired;
            }
            continue;
        }
        if ( spins_left )
        {
            // Not takeable right now: look again for a moment before committing to
            // a sleep. Reader counts under churn hit zero for an instant, and a
            // writer that announces and sleeps through every such instant costs a
            // wait/wake pair each time round (the readers pay the wake). A bounded
            // spin, no sleeping: the budget renews after every wake.
            --spins_left;
            for ( auto i{ 0 }; i < 8; ++i ) { cpu_relax(); }
            observed = state_.load( std::memory_order_relaxed );
            continue;
        }
        if ( ( observed & writer_parked_bit ) == 0 )
        {
            // Announce "about to sleep" onto the observed (not takeable) word; the
            // sleep below is on exactly that value, so whoever changes the word
            // afterwards either fails the futex re-check or sees the announcement.
            auto const desired{ state_t( observed | writer_parked_bit ) };
            if ( state_.compare_exchange_weak( observed, desired, std::memory_order_relaxed, std::memory_order_relaxed ) )
            {
                PSI_FUTEX_RW_MUTEX_COUNT( writer_announces );
                observed = desired; // successfully set; safe to park against this value
            }
            continue; // either way (won or lost the CAS), re-evaluate from the top
        }
        PSI_FUTEX_RW_MUTEX_COUNT( writer_waits );
        state_.wait_if_equal( observed, writer_wait_bits );
        observed = state_.load( std::memory_order_relaxed ); // re-arm before re-checking
        spins_left = spin_rounds;
    }
}

void futex_rw_mutex::release_rw() noexcept
{
    BOOST_ASSERT_MSG( ( state_.load( std::memory_order_relaxed ) & writer_locked_bit ) != 0, "release_rw without a matching acquire_rw" );
    auto const old_state{ state_.exchange( 0, std::memory_order_release ) };
    // No-waiter fast path (the pthread_cond_signal-style skip): every parker
    // announces itself in the state word before parking (writer_parked_bit /
    // reader_parked_bit), and the futex's atomic value re-check on park means
    // an announcement can never slip past this exchange unobserved -- so a
    // word with neither bit set proves nobody is (or can end up) parked
    // against the pre-release state, and the wake syscall can be skipped.
    // When it does fire it wakes EITHER category (either_wait_bits --
    // behaviorally identical to the default all_bits here since these are the
    // only two categories that ever park on this word; spelled out for
    // documentation).
    if ( old_state & parked_bits )
    {
        PSI_FUTEX_RW_MUTEX_COUNT( release_rw_wakes );
        state_.wake_all( either_wait_bits );
    }
}

bool futex_rw_mutex::try_acquire_rw() noexcept
{
    auto observed{ state_.load( std::memory_order_relaxed ) };
    // Same fast-path condition as acquire_rw: ignore writer_waiting_bit (see there);
    // same announce-bit preservation too (see acquire_rw).
    if ( ( observed & ( reader_mask | writer_locked_bit ) ) != 0 )
    {
        return false;
    }
    return state_.compare_exchange_strong( observed, state_t( writer_locked_bit | ( observed & parked_bits ) ), std::memory_order_acquire, std::memory_order_relaxed );
}

void futex_rw_mutex::acquire_ro_core( state_t const reader_blocking_bits ) noexcept
{
    for ( ; ; )
    {
        auto observed{ state_.load( std::memory_order_relaxed ) };
        if ( ( observed & reader_blocking_bits ) == 0 )
        {
            BOOST_ASSERT_MSG( ( observed & reader_mask ) != reader_mask, "reader_count overflow" );
            if ( state_.compare_exchange_weak( observed, state_t( observed + 1 ), std::memory_order_acquire, std::memory_order_relaxed ) )
            {
                return;
            }
        }
        else
        {
            if ( ( observed & reader_parked_bit ) == 0 )
            {
                // Announce before parking -- what makes release_rw's no-waiter
                // fast path sound (see there). Lost CAS just means the state
                // changed; either way re-evaluate from the top.
                state_.compare_exchange_weak( observed, state_t( observed | reader_parked_bit ), std::memory_order_relaxed, std::memory_order_relaxed );
                continue;
            }
            state_.wait_if_equal( observed, reader_wait_bits );
        }
    }
}

bool futex_rw_mutex::try_acquire_ro_core( state_t const reader_blocking_bits ) noexcept
{
    auto observed{ state_.load( std::memory_order_relaxed ) };
    if ( ( observed & reader_blocking_bits ) != 0 )
    {
        return false;
    }
    BOOST_ASSERT_MSG( ( observed & reader_mask ) != reader_mask, "reader_count overflow" );
    return state_.compare_exchange_strong( observed, state_t( observed + 1 ), std::memory_order_acquire, std::memory_order_relaxed );
}

void reader_preferring_futex_rw_mutex::release_ro() noexcept
{
    release_ro_core(); // no read_recursion_registry bookkeeping, see the header
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------

#undef PSI_FUTEX_RW_MUTEX_COUNT

#endif // PSI_THRD_LITE_HAS_FUTEX
