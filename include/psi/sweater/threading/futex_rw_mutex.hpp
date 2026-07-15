////////////////////////////////////////////////////////////////////////////////
///
/// \file futex_rw_mutex.hpp
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
#pragma once
//------------------------------------------------------------------------------
#include "futex.hpp"
#include "read_recursion_registry.hpp"

#include <boost/assert.hpp>

#include <climits>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// EXPERIMENTAL / research prototype -- not wired into `rw_mutex` (posix/rw_mutex.hpp
// stays pthread_rwlock-backed, windows/rw_mutex.hpp stays SRWLOCK-backed; both are
// production-proven). This is the answer to "a pointer-sized, SRWLOCK-style
// writer-favouring rwlock built on a futex, for platforms whose native rwlock
// primitive is comparatively heavy" (posix pthread_rwlock_t is 200 bytes on OSX --
// see posix/rw_mutex.hpp).
//
// Built directly on psi::thrd_lite::futex (futex.hpp), so it is only actually
// available where that primitive has a real backend: Linux (SYS_futex,
// linux/futex.cpp), Windows (WaitOnAddress/WakeByAddress, windows/futex.cpp --
// present for cross-validation/testing only; production Windows code should keep
// using SRWLOCK directly, which is a first-class OS primitive rather than one
// layered on WaitOnAddress, and is already what windows/rw_mutex.hpp uses), and
// Apple (__ulock_wait/__ulock_wake, apple/futex.cpp -- see that file's own
// design-doc comment). The Apple backend is PRIVATE API: os_unfair_lock is
// exclusive-only (no shared/reader side at all), and __ulock_wait/__ulock_wake
// (the same pair libc++ itself falls back to internally) are undocumented,
// unversioned Darwin syscalls -- the same category of risk already flagged for
// Windows SRWLOCK fairness in rrw_mutex.hpp, except there we at least have
// citations and years of stable empirical behaviour; a private syscall number
// has neither. Accordingly this whole type stays bleeding-edge/testing-only
// EVERYWHERE, and on Apple specifically it is not wired into rw_mutex/rrw_mutex
// at all -- production Apple code should keep using this codebase's existing
// pthread_rwlock-backed posix/rw_mutex.hpp. If a concrete performance need for
// Apple ever justifies shipping on __ulock, that is a separate, explicit
// decision (private-API risk), not a default.
//
// ---------------------------------------------------------------------------
// State layout
// ---------------------------------------------------------------------------
// A single futex::value_type word (uint32_t on Linux/generic; uint16_t on Windows,
// since psi::thrd_lite::futex is sized to hardware_concurrency_t there -- the
// algorithm below is written generically against that width, not hardcoded to 32
// bits):
//
//   bit  (W-1)      : writer_locked_bit  -- a writer currently holds the lock
//   bit  (W-2)      : writer_waiting_bit -- at least one writer is waiting; new
//                      readers must not admit (this is what makes the lock
//                      writer-preferring, mirroring rw_mutex's pthread NP kind)
//   bits 0..(W-3)   : reader_count       -- readers currently holding the lock
//
// Free state is exactly 0. Writer-held state has writer_locked_bit set and
// reader_count == 0 (mutually exclusive by construction: acquire_rw only
// transitions 0 -> writer_locked_bit).
//
// ---------------------------------------------------------------------------
// Algorithm (writer-preferring, non-recursive -- same contract as rw_mutex)
// ---------------------------------------------------------------------------
// acquire_ro: CAS reader_count+1 while neither writer bit is set; else park via
//   futex::wait_if_equal(observed_state) and retry from scratch. FUTEX_WAIT /
//   WaitOnAddress atomically re-checks the value before parking, so a state change
//   between our load and the wait call is never a missed wakeup.
// release_ro: fetch_sub(1); if that was the last reader AND a writer is waiting,
//   wake_one() (only a parked writer can be waiting at that point -- a reader
//   cannot be parked while another reader holds the lock and no writer is queued,
//   since acquire_ro's fast CAS path would have admitted it immediately).
// acquire_rw: if state == 0, CAS 0 -> writer_locked_bit and return. Otherwise
//   ensure writer_waiting_bit is set (best-effort CAS; failure just means someone
//   else set it or the state changed, either way retry) and park. Multiple queued
//   writers all share the one writer_waiting_bit and are woken together on
//   release (thundering-herd on multi-writer contention is a known, documented
//   simplification -- see "Known limitations" below).
// release_rw: exchange(0) and, if the old state indicates anyone could be
//   parked (which is always possible: a reader may have parked behind
//   writer_locked_bit even with writer_waiting_bit unset), wake_all(). Waking
//   unconditionally (rather than trying to track "any parked waiter" precisely)
//   trades a possible redundant syscall on an uncontended release for zero risk
//   of a missed wakeup -- deliberately the conservative choice.
//
// ---------------------------------------------------------------------------
// Known limitations (research prototype, not yet production-hardened)
// ---------------------------------------------------------------------------
// - Single writer_waiting_bit (not a count/queue): release_rw wakes ALL parked
//   threads (readers and writers alike), who then race to re-acquire; only one
//   writer wins the CAS, the rest re-park. Correct, but not contention-optimal
//   under sustained multi-writer load -- a real production version would likely
//   want a proper waiter queue (or at least a waiting-writer count) to wake
//   precisely.
// - No try_acquire timeout / no fairness ticket beyond "writer bit blocks new
//   readers": a writer that loses the CAS race after being woken goes back to
//   the tail of the same shared wait set, no strict FIFO ordering guaranteed
//   (matches futex semantics in general, not a regression vs the OS rwlocks this
//   is compared against -- neither pthread_rwlock nor SRWLOCK document strict
//   FIFO writer ordering either).
// - reader_count width is (W-2) bits: 30 bits (Linux/generic uint32_t) or 14 bits
//   (Windows uint16_t) -- effectively unbounded for any realistic thread count,
//   asserted in debug builds (see acquire_ro).
// ---------------------------------------------------------------------------

class futex_rw_mutex
{
public:
    using state_t = futex::value_type;

    void acquire_ro() noexcept
    {
        detail::on_ro_acquire( this ); // writer-preferring: nested read is the documented hang, same as rw_mutex
        for ( ; ; )
        {
            auto observed{ state_.load( std::memory_order_relaxed ) };
            if ( ( observed & ( writer_locked_bit | writer_waiting_bit ) ) == 0 )
            {
                BOOST_ASSERT_MSG( ( observed & reader_mask ) != reader_mask, "reader_count overflow" );
                if ( state_.compare_exchange_weak( observed, state_t( observed + 1 ), std::memory_order_acquire, std::memory_order_relaxed ) )
                {
                    return;
                }
            }
            else
            {
                state_.wait_if_equal( observed );
            }
        }
    }

    void release_ro() noexcept
    {
        detail::on_ro_release( this );
        auto const old{ state_.fetch_sub( 1, std::memory_order_release ) };
        BOOST_ASSERT_MSG( ( old & reader_mask ) != 0, "release_ro without a matching acquire_ro" );
        if ( ( ( old & reader_mask ) == 1 ) && ( old & writer_waiting_bit ) )
        {
            // wake_all, not wake_one: readers that saw writer_waiting_bit set also park
            // on this SAME futex word (see acquire_ro), so a single wake_one() can be
            // "stolen" by one of THEM instead of the writer it was meant for -- that
            // reader just re-parks (writer_waiting_bit is still set from its point of
            // view), the wake is wasted, and the writer can be left permanently parked
            // if nothing else ever wakes this word again (observed as an intermittent
            // hang under StressMutualExclusionAndProgress before this fix). wake_all
            // avoids the ambiguity entirely, at the cost of the woken non-writers just
            // re-checking and re-parking -- the same conservative tradeoff release_rw
            // already makes.
            state_.wake_all();
        }
    }

    bool try_acquire_ro() noexcept
    {
        auto observed{ state_.load( std::memory_order_relaxed ) };
        if ( ( observed & ( writer_locked_bit | writer_waiting_bit ) ) != 0 )
        {
            return false;
        }
        BOOST_ASSERT_MSG( ( observed & reader_mask ) != reader_mask, "reader_count overflow" );
        if ( !state_.compare_exchange_strong( observed, state_t( observed + 1 ), std::memory_order_acquire, std::memory_order_relaxed ) )
        {
            return false;
        }
        detail::on_ro_acquire( this );
        return true;
    }

    void acquire_rw() noexcept
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
                // Also clears writer_waiting_bit (transition target is writer_locked_bit
                // alone): we're no longer "waiting", we're holding.
                if ( state_.compare_exchange_weak( observed, writer_locked_bit, std::memory_order_acquire, std::memory_order_relaxed ) )
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
            state_.wait_if_equal( observed );
            observed = state_.load( std::memory_order_relaxed ); // re-arm before re-checking
        }
    }

    void release_rw() noexcept
    {
        BOOST_ASSERT_MSG( ( state_.load( std::memory_order_relaxed ) & writer_locked_bit ) != 0, "release_rw without a matching acquire_rw" );
        state_.exchange( 0, std::memory_order_release );
        // Always wake_all: a reader can be parked behind writer_locked_bit alone
        // (writer_waiting_bit unset), so its absence does not mean no one is
        // waiting -- see "release_rw" in the algorithm comment above.
        state_.wake_all();
    }

    bool try_acquire_rw() noexcept
    {
        auto observed{ state_.load( std::memory_order_relaxed ) };
        // Same fast-path condition as acquire_rw: ignore writer_waiting_bit (see there).
        if ( ( observed & ( reader_mask | writer_locked_bit ) ) != 0 )
        {
            return false;
        }
        return state_.compare_exchange_strong( observed, writer_locked_bit, std::memory_order_acquire, std::memory_order_relaxed );
    }

    // debugging aid, mirrors rw_mutex::is_locked
    bool is_locked() const noexcept { return state_.load( std::memory_order_relaxed ) != 0; }

public: // std::shared_lock interface
    void   lock() noexcept { acquire_rw(); }
    void unlock() noexcept { release_rw(); }
    bool try_lock() noexcept { return try_acquire_rw(); }

    void   lock_shared() noexcept { acquire_ro(); }
    void unlock_shared() noexcept { release_ro(); }
    bool try_lock_shared() noexcept { return try_acquire_ro(); }

private:
    static constexpr unsigned  state_bits          = sizeof( state_t ) * CHAR_BIT;
    static constexpr state_t   writer_locked_bit    = state_t( state_t{ 1 } << ( state_bits - 1 ) );
    static constexpr state_t   writer_waiting_bit   = state_t( state_t{ 1 } << ( state_bits - 2 ) );
    static constexpr state_t   reader_mask          = state_t( writer_waiting_bit - 1 );

    futex state_ = { 0 };
}; // class futex_rw_mutex

// NOTE: rw_preference.hpp (PR #17) landed as tag TYPES selected at construction,
// not a runtime-queryable trait -- there is nothing for a type like this one
// (which only ever implements the writer-preferring algorithm) to specialize or
// register. It is simply never constructed with reader_preferring_t; nothing
// further to wire up here.

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
