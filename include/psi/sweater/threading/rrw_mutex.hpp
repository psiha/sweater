////////////////////////////////////////////////////////////////////////////////
///
/// \file rrw_mutex.hpp
/// -------------------
///
/// (c) Copyright Domagoj Saric.
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
#include "rw_mutex.hpp"
#include "read_recursion_registry.hpp"

#include <vector>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
//
// rrw_mutex - a reentrant-READ extension of rw_mutex (recursive read, non-recursive
// write).
//
// ---------------------------------------------------------------------------
// The underlying-primitive limitation this works around
// ---------------------------------------------------------------------------
// rw_mutex wraps the platform shared/exclusive lock with default attributes:
// writer-preferring and NON-recursive. Recursively taking the *read* side while a
// writer is queued is documented as a hang on every backend:
//
//   - POSIX pthread_rwlock_t: a thread may hold multiple concurrent read locks, but
//     if it requests another read lock "while a writer is waiting on the rwlock, the
//     calling thread may deadlock" (writer preference wins over the nested reader).
//     PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP is strictly writer-preferring.
//       https://pubs.opengroup.org/onlinepubs/9699919799/functions/pthread_rwlock_rdlock.html
//       https://man7.org/linux/man-pages/man3/pthread_rwlockattr_setkind_np.3.html
//   - Windows SRWLOCK: "SRW locks ... cannot be acquired recursively." Acquiring a
//     shared lock a thread already holds (shared or exclusive) is undefined and
//     deadlocks in practice.
//       https://learn.microsoft.com/en-us/windows/win32/sync/slim-reader-writer--srw--locks
//       https://learn.microsoft.com/en-us/windows/win32/api/synchronization/nf-synchronization-acquiresrwlockshared
//
// rw_mutex itself asserts (debug builds) on recursive read-acquisition so the misuse
// fails loudly rather than hanging; rrw_mutex is the type to reach for when a design
// legitimately needs nested reads.
//
// ---------------------------------------------------------------------------
// How it works
// ---------------------------------------------------------------------------
// Only the OUTERMOST read acquire on a given mutex (per thread) touches the OS rwlock;
// nested acquires and all but the last release just adjust this thread's depth counter.
// The OS read lock is therefore held for the union of all live read holds on the thread
// -- so relocation/pinning guarantees that depend on holding the read lock are preserved
// -- while a nested acquire never re-enters the OS rdlock and so cannot block behind a
// pending writer. The write side is inherited UNCHANGED (non-recursive): the hang is
// specific to nested *read* acquisition behind a queued writer.
//
// ---------------------------------------------------------------------------
// Why the recursion state is per-thread (not a counter member on the mutex)
// ---------------------------------------------------------------------------
// Reentrancy needs per-(thread, mutex) state: "does THIS thread already hold a read
// lock on THIS mutex?". A read lock is multi-owner, so a single scalar count on the
// mutex cannot answer it (count==2 might be one thread twice -> skip the OS re-acquire,
// or two threads once each -> both must really hold). A single shared counter is also
// racy (a second thread that bumps the count and proceeds before any thread's OS read
// lock is actually held races a writer) and starvation-prone. A correct on-the-mutex
// form -- a per-worker-slot depth array indexed by thread id -- would false-share the
// hot lock word across workers and bloat every mutex instance. Per-thread storage has
// neither cost: each thread's table is private memory. C++ has no per-object
// thread_local, so the table is a thread_local keyed by mutex identity (see
// read_recursion_registry), shared by all rrw_mutex instances of a given HeldVec type.
// This is also how mainstream reentrant read locks do it: .NET ReaderWriterLockSlim
// (recursion mode) keeps a per-thread linked list of per-lock counts, Java
// ReentrantReadWriteLock keeps per-thread hold counters in a ThreadLocal.
//
// Considered alternative: SRWLOCK-style intrusive nodes on the caller's stack (no
// separate container at all). That does not transplant here: SRWLOCK's wait blocks only
// need to live for the DURATION OF THE BLOCKING WAIT, so a local in the waiting frame
// suffices. A reentrancy record must live for the DURATION OF THE HOLD, so the node
// would have to be embedded in the guard object -- which (a) leaves the raw
// acquire_ro()/lock_shared() (std::shared_lock-compatible) API with nowhere to put it,
// and (b) since guards are movable and routinely stored/returned (long-lived pins),
// every guard move would have to re-link a per-thread list. And it would not remove the
// search either: "is this mutex already held by this thread" is still a walk over the
// thread's live holds. The registry walk is over that same live-hold set -- typically a
// handful of entries, contiguous in private per-thread memory, allocation-free with an
// SBO HeldVec -- so the intrusive variant saves nothing and costs API and move
// complexity.
//
// Correctness rests on acquire/release being balanced on the SAME thread per mutex.
//
// ---------------------------------------------------------------------------
// HeldVec
// ---------------------------------------------------------------------------
// The per-thread held-list container is a template parameter so callers pick the
// storage / allocation policy without dragging a container dependency into sweater:
// the default `rrw_mutex` alias uses std::vector (dependency-light, standalone), while
// a caller that already has a small-vector can instantiate basic_rrw_mutex with an SBO
// container to keep the shallow common case allocation-free.
//
////////////////////////////////////////////////////////////////////////////////

template <class HeldVec>
class
// Match the base's conditional trivial_abi: basic_rrw_mutex adds no data members, so it
// is trivially relocatable exactly when rw_mutex is. This matters for callers that
// bitwise-relocate objects embedding the mutex (trivial relocation is deduced from
// trivial_abi).
#if defined( _WIN32 ) || defined( PTHREAD_RWLOCK_INITIALIZER )
    [[ clang::trivial_abi ]]
#endif
basic_rrw_mutex : public rw_mutex
{
public:
    using rw_mutex::rw_mutex;

    void acquire_ro() PSI_NOEXCEPT_EXCEPT_BADALLOC
    {
        verify_deadlock(); // read-while-holding-the-exclusive-side still deadlocks: only *read* recursion is handled
        if ( registry().enter( this ) )
        {
            os_acquire_ro();
        }
    }
    void release_ro() noexcept
    {
        if ( registry().leave( this ) )
        {
            os_release_ro();
        }
    }

    bool try_acquire_ro() PSI_NOEXCEPT_EXCEPT_BADALLOC
    {
        verify_deadlock();
        if ( registry().bump_if_held( this ) ) // reentrant try: already held -> succeed
        {
            return true;
        }
        if ( os_try_acquire_ro() )
        {
            try
            {
                registry().note_first( this );
            }
            catch ( ... ) // never leak the OS read lock if hold-tracking allocation fails
            {
                os_release_ro();
                throw;
            }
            return true;
        }
        return false;
    }

    // std::shared_lock interface -- re-route through the reentrant overrides above (the
    // base's non-virtual forwarders would statically bind to the base, non-reentrant,
    // acquire_ro/release_ro).
    void   lock_shared() PSI_NOEXCEPT_EXCEPT_BADALLOC { acquire_ro(); }
    void unlock_shared() noexcept                     { release_ro(); }
    bool try_lock_shared() PSI_NOEXCEPT_EXCEPT_BADALLOC { return try_acquire_ro(); }

    // Write side (acquire_rw / release_rw / lock / unlock / try_lock / try_acquire_rw)
    // is inherited UNCHANGED from rw_mutex: non-recursive, as before.

private:
    [[ nodiscard ]] static detail::read_recursion_registry<HeldVec> & registry() noexcept
    {
        return detail::read_recursion_registry<HeldVec>::tls();
    }
}; // class basic_rrw_mutex

// Dependency-light default (std::vector storage) for standalone use; consumers that
// already depend on a small-vector can supply an SBO container as HeldVec to keep the
// shallow common case allocation-free.
using rrw_mutex = basic_rrw_mutex< std::vector<detail::read_hold> >;

// Reentrant read guard for the default rrw_mutex. (The write guard rw_lock works for
// any basic_rrw_mutex unchanged, by upcast to rw_mutex.)
using rro_lock = basic_ro_lock<rrw_mutex>;

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
