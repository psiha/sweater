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
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// Out-of-line on purpose (hot-path inlining across the header boundary is
// LTO's job): the header keeps only the trivial wrappers and the design doc.

void futex_rw_mutex::release_ro() noexcept
{
    detail::on_ro_release( this );
    auto const old{ state_.fetch_sub( 1, std::memory_order_release ) };
    BOOST_ASSERT_MSG( ( old & reader_mask ) != 0, "release_ro without a matching acquire_ro" );
    if ( ( ( old & reader_mask ) == 1 ) && ( old & writer_waiting_bit ) )
    {
        // Bitset-targeted at writer_wait_bits (see the header's design-doc comment): on
        // backends without a real bitset (all but Linux) this degrades to plain
        // wake_all's old behaviour -- readers that saw writer_waiting_bit set also
        // park on this SAME futex word (see acquire_ro), so an untargeted wake_all()
        // (or a naive wake_one(), which is what this originally tried) can reach/be
        // stolen by one of THEM instead of a writer -- that reader just re-parks, the
        // wake is wasted, and on wake_one() specifically a writer can be left
        // permanently parked if nothing else ever wakes this word again (observed as
        // an intermittent hang under StressMutualExclusionAndProgress before the
        // wake_all fix). The bitset now recovers wake_one's CATEGORY precision (wakes
        // only writers, never readers) on the one backend that supports it, without
        // wake_one's single-thread-only risk of picking the wrong category -- still
        // wakes every parked writer, not just one (see "Known limitations" in the
        // header for why that's an accepted simplification, not a bitset limitation).
        state_.wake_all( writer_wait_bits );
    }
}

void futex_rw_mutex::acquire_rw() noexcept
{
    auto observed{ state_.load( std::memory_order_relaxed ) };
    for ( ; ; )
    {
        // "Free to take" means no readers AND no writer currently holding --
        // writer_waiting_bit may already be set (by us, on a previous iteration of
        // this very loop, or by another queued writer) and that must NOT block us:
        // requiring the whole word to be exactly 0 here is the bug this replaced --
        // a writer that had set its own writer_waiting_bit could never again observe
        // state 0 once readers drained (the bit it set itself permanently excluded
        // the very state it was waiting for -- a self-inflicted missed wakeup).
        if ( ( observed & ( reader_mask | writer_locked_bit ) ) == 0 )
        {
            // PRESERVE the announce bits (writer_waiting_bit may cover OTHER
            // writers still parked on it; reader_parked_bit likewise): with
            // release_rw's wake now conditional on them, clearing either here
            // would strand those parked threads. The bits are cleared only by
            // release_rw's exchange( 0 ). (A sole writer that announced and
            // then won thus carries its own stale waiting bit through the
            // hold -- costing release one possibly-redundant wake, exactly
            // the unconditional behaviour this fast path replaces.)
            if ( state_.compare_exchange_weak( observed, state_t( writer_locked_bit | ( observed & parked_bits ) ), std::memory_order_acquire, std::memory_order_relaxed ) )
            {
                return;
            }
            continue; // observed refreshed by the failed CAS; retry
        }
        if ( ( observed & writer_waiting_bit ) == 0 )
        {
            auto const desired{ state_t( observed | writer_waiting_bit ) };
            if ( state_.compare_exchange_weak( observed, desired, std::memory_order_relaxed, std::memory_order_relaxed ) )
            {
                observed = desired; // successfully set; safe to park against this value
            }
            continue; // either way (won or lost the CAS), re-evaluate from the top
        }
        state_.wait_if_equal( observed, writer_wait_bits );
        observed = state_.load( std::memory_order_relaxed ); // re-arm before re-checking
    }
}

void futex_rw_mutex::release_rw() noexcept
{
    BOOST_ASSERT_MSG( ( state_.load( std::memory_order_relaxed ) & writer_locked_bit ) != 0, "release_rw without a matching acquire_rw" );
    auto const old_state{ state_.exchange( 0, std::memory_order_release ) };
    // No-waiter fast path (the pthread_cond_signal-style skip): every parker
    // announces itself in the state word before parking (writer_waiting_bit /
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
    auto const old{ state_.fetch_sub( 1, std::memory_order_release ) };
    BOOST_ASSERT_MSG( ( old & reader_mask ) != 0, "release_ro without a matching acquire_ro" );
    if ( ( ( old & reader_mask ) == 1 ) && ( old & writer_waiting_bit ) )
    {
        // Bitset-targeted at writer_wait_bits, same reasoning as the base's
        // release_ro -- doubly safe here, since under reader preference no OTHER
        // reader can ever be parked at this instant either way (they never wait on
        // writer_waiting_bit alone, only on an actual writer_locked_bit hold, which
        // cannot be set concurrently with this reader's own hold just now ending).
        state_.wake_all( writer_wait_bits );
    }
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
#endif // PSI_THRD_LITE_HAS_FUTEX
