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
// see posix/rw_mutex.hpp). Two variants are defined: futex_rw_mutex itself (writer-
// preferring, aliased below as writer_preferring_futex_rw_mutex) and
// reader_preferring_futex_rw_mutex (a subclass reimplementing the read side against
// the same state word -- see its own doc comment further down).
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
// Wake targeting via futex bitsets (Linux only, transparently ignored elsewhere)
// ---------------------------------------------------------------------------
// psi::thrd_lite::futex's wait_if_equal/wake_all take an optional bitset (futex.hpp;
// on Linux, backed by FUTEX_WAIT_BITSET/FUTEX_WAKE_BITSET; a no-op elsewhere). Parked
// readers listen on reader_wait_bits, parked writers on writer_wait_bits (two
// independent bits, unrelated to the state-word bits above -- the bitset is the
// syscall's separate val3 argument, not part of the compared/stored value). This lets
// release_ro's wake target ONLY parked writers precisely, instead of waking every
// parked thread and making the non-writers re-check and re-park for nothing:
// release_ro only ever wakes when the reader count is hitting zero, at which instant
// writer_locked_bit cannot be set (mutual exclusion with active readers), so any
// reader parked at that exact moment can only be one that arrived AFTER
// writer_waiting_bit was set (queued behind the same writer this wake is FOR) --
// waking it too would be pure waste, never a missed wakeup. release_rw still wakes
// EITHER category (either_wait_bits): after an exclusive release, both a queued
// writer and a fresh reader parked behind writer_locked_bit alone are eligible.
//
// ---------------------------------------------------------------------------
// Known limitations (research prototype, not yet production-hardened)
// ---------------------------------------------------------------------------
// - Single writer_waiting_bit (not a count/queue): release_rw wakes ALL parked
//   writers (bitset-targeted, but still every one of them, not just one), who then
//   race to re-acquire; only one wins the CAS, the rest re-park. Correct, but not
//   contention-optimal under sustained multi-writer load -- a real production
//   version would likely want a proper waiter queue (or at least a waiting-writer
//   count) to wake exactly one.
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

    futex_rw_mutex() noexcept = default;

    // Mirrors rw_mutex's copy/move ctors (posix/rw_mutex.hpp): futex's std::atomic base
    // has a deleted copy ctor, so without these, any type embedding a futex_rw_mutex
    // member (e.g. BitmapIndex's PlainRWMutex on Apple) would have its own copy/move
    // ctors implicitly deleted too. A "copy" here just default-constructs a fresh,
    // dormant instance -- the same "allow copy so as to enable use of compiler
    // generated constructors/functions for types that contain rw_mutex members"
    // rationale, not an actual state copy (there is no valid notion of copying live
    // lock state).
    explicit
    futex_rw_mutex( [[ maybe_unused ]] futex_rw_mutex const &  other ) noexcept : futex_rw_mutex{} { BOOST_ASSERT_MSG( !other.is_locked(), "Copy allowed only for dormant mutexes" ); }
    futex_rw_mutex( [[ maybe_unused ]] futex_rw_mutex       && other ) noexcept : futex_rw_mutex{} { BOOST_ASSERT_MSG( !other.is_locked(), "Relocation allowed only for dormant mutexes" ); }

    futex_rw_mutex & operator=( [[ maybe_unused ]] futex_rw_mutex && other ) noexcept
    {
        BOOST_ASSERT_MSG( !is_locked() && !other.is_locked(), "Relocation allowed only for dormant mutexes" );
        return *this;
    }

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
                state_.wait_if_equal( observed, reader_wait_bits );
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
            // Bitset-targeted at writer_wait_bits (see the design-doc comment above): on
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
            // wakes every parked writer, not just one (see "Known limitations" above for
            // why that's an accepted simplification, not a bitset limitation).
            state_.wake_all( writer_wait_bits );
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
            state_.wait_if_equal( observed, writer_wait_bits );
            observed = state_.load( std::memory_order_relaxed ); // re-arm before re-checking
        }
    }

    void release_rw() noexcept
    {
        BOOST_ASSERT_MSG( ( state_.load( std::memory_order_relaxed ) & writer_locked_bit ) != 0, "release_rw without a matching acquire_rw" );
        state_.exchange( 0, std::memory_order_release );
        // Always wake EITHER category: a reader can be parked behind writer_locked_bit
        // alone (writer_waiting_bit unset), so its absence does not mean no one is
        // waiting -- see "release_rw" in the algorithm comment above. (either_wait_bits
        // is behaviorally identical to the default all_bits here since these are the
        // only two categories that ever park on this word -- spelled out explicitly for
        // documentation, not because it differs.)
        state_.wake_all( either_wait_bits );
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

protected: // exposed for reader_preferring_futex_rw_mutex below, which reimplements the
           // read side against this same state word/bit layout
    static constexpr unsigned  state_bits          = sizeof( state_t ) * CHAR_BIT;
    static constexpr state_t   writer_locked_bit    = state_t( state_t{ 1 } << ( state_bits - 1 ) );
    static constexpr state_t   writer_waiting_bit   = state_t( state_t{ 1 } << ( state_bits - 2 ) );
    static constexpr state_t   reader_mask          = state_t( writer_waiting_bit - 1 );

    // futex::wait_if_equal/wake_all bitset categories (see the design-doc comment
    // above) -- these live in the syscall's separate bitset argument, not the state
    // word above, so they don't need to avoid colliding with writer_locked_bit/
    // writer_waiting_bit/reader_mask; any two distinct, non-zero bits would do.
    static constexpr futex::value_type reader_wait_bits = futex::value_type{ 0b01 };
    static constexpr futex::value_type writer_wait_bits = futex::value_type{ 0b10 };
    static constexpr futex::value_type either_wait_bits = futex::value_type( reader_wait_bits | writer_wait_bits );

    futex state_ = { 0 };
}; // class futex_rw_mutex

// writer_preferring_futex_rw_mutex: futex_rw_mutex's default (and only, until the
// subclass below) algorithm already IS writer-preferring -- this alias exists purely so
// callers that want to name the preference explicitly can, symmetric with
// writer_preferring_rw_mutex (rw_mutex.hpp) / reader_preferring_rw_mutex (posix/
// rw_mutex.hpp).
using writer_preferring_futex_rw_mutex = futex_rw_mutex;

// ---------------------------------------------------------------------------
// reader_preferring_futex_rw_mutex
// ---------------------------------------------------------------------------
// Reader-preferring variant of futex_rw_mutex, mirroring the writer_preferring_rw_mutex
// / reader_preferring_rw_mutex split in posix/rw_mutex.hpp -- EXCEPT unlike that pair
// (where the OS's own rwlock "kind" flag does the actual admission-policy work and the
// derived class only skips a now-inapplicable debug tripwire), futex_rw_mutex implements
// its own admission policy directly in the CAS condition: there is no "kind" to flip at
// construction. This subclass therefore reimplements acquire_ro/release_ro/
// try_acquire_ro against the SAME state word/bit layout as the base -- built entirely on
// the futex machinery already in play, with no separate TLS hold-tracking container and
// no read_recursion_registry involvement at all (not even the debug tripwire the base
// pays for -- see below for why it doesn't apply here).
//
// New readers are admitted whenever no writer currently HOLDS the lock (writer_locked_
// bit), ignoring writer_waiting_bit entirely -- a queued writer never blocks a new
// reader. That is also what makes same-thread nested reads natively deadlock-free here:
// a thread already holding a read lock can never observe writer_locked_bit become set
// while it holds (the writer's own fast-path CAS requires reader_mask == 0 first), so a
// nested acquire_ro() on the same thread just re-takes the fast CAS path. That is why,
// like posix's reader_preferring_rw_mutex, this skips read_recursion_registry's
// instrumentation (detail::on_ro_acquire/on_ro_release) entirely rather than tripping
// its now-inapplicable same-thread-recursion assert.
//
// The write side (acquire_rw/release_rw/try_acquire_rw) is inherited UNCHANGED: a
// writer's fast-path CAS condition and its wake-on-release target don't depend on the
// read-admission policy, only on the actual state transitions, which are identical
// either way.
//
// Same "writer can starve under sustained/bursty read load" caveat that applies to the
// OS-level posix reader_preferring_rw_mutex applies here too -- accepted by whoever
// opts into this type, same as there.
class reader_preferring_futex_rw_mutex : public futex_rw_mutex
{
public:
    void acquire_ro() noexcept
    {
        for ( ; ; )
        {
            auto observed{ state_.load( std::memory_order_relaxed ) };
            if ( ( observed & writer_locked_bit ) == 0 )
            {
                BOOST_ASSERT_MSG( ( observed & reader_mask ) != reader_mask, "reader_count overflow" );
                if ( state_.compare_exchange_weak( observed, state_t( observed + 1 ), std::memory_order_acquire, std::memory_order_relaxed ) )
                {
                    return;
                }
            }
            else
            {
                state_.wait_if_equal( observed, reader_wait_bits );
            }
        }
    }

    // Skips detail::on_ro_release (unlike the base): this type never registers with
    // read_recursion_registry in acquire_ro above, so there is nothing to balance here.
    void release_ro() noexcept
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

    bool try_acquire_ro() noexcept
    {
        auto observed{ state_.load( std::memory_order_relaxed ) };
        if ( ( observed & writer_locked_bit ) != 0 )
        {
            return false;
        }
        BOOST_ASSERT_MSG( ( observed & reader_mask ) != reader_mask, "reader_count overflow" );
        return state_.compare_exchange_strong( observed, state_t( observed + 1 ), std::memory_order_acquire, std::memory_order_relaxed );
    }

    // std::shared_lock interface: re-route through the shadowed overrides above (the
    // base's non-virtual lock_shared()/unlock_shared()/try_lock_shared() would
    // statically bind to the base's acquire_ro/release_ro/try_acquire_ro).
    void   lock_shared() noexcept { acquire_ro(); }
    void unlock_shared() noexcept { release_ro(); }
    bool try_lock_shared() noexcept { return try_acquire_ro(); }
}; // class reader_preferring_futex_rw_mutex

// NOTE: rw_preference.hpp (PR #17) landed as tag TYPES selected at construction, not a
// runtime-queryable trait -- there is nothing for either futex_rw_mutex or
// reader_preferring_futex_rw_mutex (each only ever implementing ONE fixed algorithm) to
// specialize or register against it. Callers select the preference by naming the type,
// same as writer_preferring_rw_mutex vs reader_preferring_rw_mutex.

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
