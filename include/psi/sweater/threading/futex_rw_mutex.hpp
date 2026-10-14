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
#include <cstdint>
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
// Apple -- macOS ONLY (__ulock_wait/__ulock_wake, apple/futex.cpp -- see that
// file's own design-doc comment; Apple's embedded OSes cannot ship private
// syscalls at all, so they have no futex backend and none of the futex-backed
// types -- see futex.hpp's PSI_THRD_LITE_HAS_FUTEX). The Apple backend is
// PRIVATE API: os_unfair_lock is
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
//   bit  (W-2)      : writer_waiting_bit -- at least one writer wants the lock; new
//                      readers must not admit (this is what makes the lock
//                      writer-preferring, mirroring rw_mutex's pthread NP kind).
//                      Admission policy only: it plays no part in wake decisions.
//   bit  (W-3)      : reader_parked_bit  -- a reader announced it is about to sleep
//   bit  (W-4)      : writer_parked_bit  -- a writer announced it is about to sleep
//   bits 0..(W-5)   : reader_count       -- readers currently holding the lock
//
// Free state is exactly 0. Writer-held state has writer_locked_bit set and
// reader_count == 0 (mutually exclusive by construction: acquire_rw only
// transitions to writer_locked_bit from a state with reader_count == 0).
//
// ---------------------------------------------------------------------------
// Algorithm (writer-preferring, non-recursive -- same contract as rw_mutex)
// ---------------------------------------------------------------------------
// acquire_ro: CAS reader_count+1 while neither writer bit is set; else announce
//   (set reader_parked_bit) and park via futex::wait_if_equal(observed_state),
//   then retry from scratch. FUTEX_WAIT /
//   WaitOnAddress atomically re-checks the value before parking, so a state change
//   between our load and the wait call is never a missed wakeup.
// release_ro: fetch_sub(1); if that was the last reader AND writer_parked_bit is
//   set, fetch_and(~writer_parked_bit) and wake_all() only if THIS call was the one
//   that cleared the bit. At most one wake per writer sleep announcement, and no
//   syscall at all while no writer is about to sleep -- however often the reader
//   count passes through zero (it does so constantly under reader preference).
// acquire_rw: if reader_count == 0 and the lock is not held, CAS to
//   writer_locked_bit and return. Otherwise set writer_waiting_bit (from the first
//   failed attempt on, so writer preference starts immediately), re-read the word for
//   a bounded number of spins (readers under churn pass through zero for an instant;
//   a writer that sleeps through each such instant costs a wait/wake pair every time
//   round), then announce (CAS-set writer_parked_bit onto the observed word; a lost
//   CAS means the word changed, retry) and park on the announced value. Multiple
//   queued writers share the bits and are all woken together (thundering-herd on
//   multi-writer contention is a known, documented simplification -- see "Known
//   limitations" below).
// release_rw: exchange(0); wake_all() only when the exchanged-out state carries a
//   parked-announcement bit (reader_parked_bit or writer_parked_bit -- every
//   parker sets its category's bit before parking, so an uncontended release
//   skips the wake syscall entirely: the same no-waiter fast path
//   pthread_cond_signal has). A writer taking the lock preserves the announce
//   bits (others may still be parked on them); they are cleared by release_rw or,
//   for writer_parked_bit, by the release_ro that wakes the writers.
//
// No lost wakeup: a parker sleeps only in wait_if_equal( v ) with its announce bit
// in v, and the futex re-checks the word against v atomically with going to sleep.
// The bit leaves the word only through a clearing read-modify-write
// (release_ro's fetch_and, release_rw's exchange), and the clearer wakes everyone
// listening if it saw the bit set. So either the clear precedes the sleep (the
// re-check fails, the parker re-evaluates) or it follows it (the clearer's wake
// finds the parker). Only one clearer sees a given bit set, hence at most one wake
// per announcement. The last reader's fetch_sub reads every announcement CASed in
// before it; a later announcement CASes against a reader_count that fetch_sub
// already changed and fails (or, after another reader entered, is released by that
// reader). A woken writer that finds the lock still unavailable simply announces
// again.
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
// waking it too would be pure waste, never a missed wakeup. (Where the bitset is
// ignored every parked thread is woken instead: once per writer sleep, bounded.) release_rw still wakes
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
// - reader_count width is (W-4) bits: 28 bits (Linux/generic uint32_t) or 12 bits
//   (Windows uint16_t) -- effectively unbounded for any realistic thread count,
//   asserted in debug builds (see acquire_ro).
// - A word that keeps changing (readers entering/leaving) makes a parking writer's
//   futex re-check fail, so the writer loops through wait_if_equal: syscalls issued
//   by the writer itself, never by the readers (their wake condition is above).
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
        acquire_ro_core( writer_locked_bit | writer_waiting_bit );
    }

    void release_ro() noexcept;

    bool try_acquire_ro() noexcept
    {
        if ( !try_acquire_ro_core( writer_locked_bit | writer_waiting_bit ) )
        {
            return false;
        }
        detail::on_ro_acquire( this );
        return true;
    }

    void acquire_rw() noexcept;

    void release_rw() noexcept;

    bool try_acquire_rw() noexcept;

    // debugging aid, mirrors rw_mutex::is_locked
    bool is_locked() const noexcept { return state_.load( std::memory_order_relaxed ) != 0; }

public: // std::shared_lock interface
    void   lock() noexcept { acquire_rw(); }
    void unlock() noexcept { release_rw(); }
    bool try_lock() noexcept { return try_acquire_rw(); }

    void   lock_shared() noexcept { acquire_ro(); }
    void unlock_shared() noexcept { release_ro(); }
    bool try_lock_shared() noexcept { return try_acquire_ro(); }

protected:
    // Shared read-side cores for this class and its admission-policy subclasses --
    // the variants differ ONLY in which writer bits block a new reader
    // (`reader_blocking_bits`: both writer bits for the writer-preferring base,
    // writer_locked_bit alone for the reader-preferring subclass); the counting,
    // park-with-announce (see release_rw's no-waiter fast path) and retry
    // mechanics are identical. A plain runtime parameter: LTO inlines the cores
    // (futex_rw_mutex.cpp) into the (per-policy) public wrappers where the
    // constant folds anyway -- a template parameter would only force two
    // instantiations.
    void acquire_ro_core( state_t reader_blocking_bits ) noexcept;

    bool try_acquire_ro_core( state_t reader_blocking_bits ) noexcept;

    // The read-release step shared by both admission policies (they differ only in
    // the read-recursion bookkeeping around it): drops one read hold and wakes the
    // announced writers if that was the last one.
    void release_ro_core() noexcept;

protected: // exposed for reader_preferring_futex_rw_mutex below, which reimplements the
           // read side against this same state word/bit layout
    static constexpr unsigned  state_bits          = sizeof( state_t ) * CHAR_BIT;
    static constexpr state_t   writer_locked_bit    = state_t( state_t{ 1 } << ( state_bits - 1 ) );
    static constexpr state_t   writer_waiting_bit   = state_t( state_t{ 1 } << ( state_bits - 2 ) );
    static constexpr state_t   reader_parked_bit    = state_t( state_t{ 1 } << ( state_bits - 3 ) ); // a reader blocked behind a writer announced itself (enables the no-waiter fast paths)
    static constexpr state_t   writer_parked_bit    = state_t( state_t{ 1 } << ( state_bits - 4 ) ); // a writer blocked behind readers/a writer announced itself (see release_ro_core)
    static constexpr state_t   parked_bits          = state_t( reader_parked_bit | writer_parked_bit );
    static constexpr state_t   reader_mask          = state_t( writer_parked_bit - 1 );
    static_assert( reader_mask >= 0xF, "the futex word is too narrow for the four flag bits plus a usable reader_count" ); // 28 bits with a 32-bit word, 12 with the 16-bit Windows one

    // futex::wait_if_equal/wake_all bitset categories (see the design-doc comment
    // above) -- these live in the syscall's separate bitset argument, not the state
    // word above, so they don't need to avoid colliding with writer_locked_bit/
    // writer_waiting_bit/reader_mask; any two distinct, non-zero bits would do.
    static constexpr futex::value_type reader_wait_bits = futex::value_type{ 0b01 };
    static constexpr futex::value_type writer_wait_bits = futex::value_type{ 0b10 };
    static constexpr futex::value_type either_wait_bits = futex::value_type( reader_wait_bits | writer_wait_bits );

    futex state_ = { 0 };
}; // class futex_rw_mutex

#ifdef PSI_THRD_LITE_FUTEX_RW_MUTEX_COUNTERS
// Test-only observation hook, compiled in only where futex_rw_mutex.cpp is built with
// this macro (the library proper never is): counts the lock's own futex traffic.
struct futex_rw_mutex_counters
{
    std::atomic<std::uint64_t> writer_announces; // writer_parked_bit CAS-set by acquire_rw
    std::atomic<std::uint64_t> writer_waits    ; // wait_if_equal calls issued by acquire_rw
    std::atomic<std::uint64_t> release_ro_wakes; // wake_all calls issued by release_ro
    std::atomic<std::uint64_t> release_rw_wakes; // wake_all calls issued by release_rw
};
extern futex_rw_mutex_counters futex_rw_mutex_call_counters;
#endif

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
        // No on_ro_acquire (see the class comment); admission defers to an actual
        // writer HOLD only -- a queued writer never blocks a new reader.
        acquire_ro_core( writer_locked_bit );
    }

    // Skips detail::on_ro_release (unlike the base): this type never registers with
    // read_recursion_registry in acquire_ro above, so there is nothing to balance here.
    void release_ro() noexcept;

    bool try_acquire_ro() noexcept
    {
        return try_acquire_ro_core( writer_locked_bit );
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
