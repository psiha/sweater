////////////////////////////////////////////////////////////////////////////////
///
/// \file rw_mutex.hpp
/// ------------------
///
/// (c) Copyright Domagoj Saric 2024 - 2026.
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
#include <psi/sweater/threading/read_recursion_registry.hpp>
#include <psi/sweater/threading/rw_preference.hpp>

#include <boost/assert.hpp>

#include <pthread.h>
#ifndef NDEBUG
#include <cstring>
#endif
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// https://probablydance.com/2019/12/30/measuring-mutexes-spinlocks-and-how-bad-the-linux-scheduler-really-is
// https://www.realworldtech.com/forum/?threadid=189711&curpostid=189723 No nuances, just buggy code (was: related to Spinlock implementation and the Linux Scheduler)
// https://github.com/markwaterman/MutexShootout
// https://github.com/nicowilliams/ctp RCU
// https://arxiv.org/abs/1109.2638 Light-weight Locks
// https://arxiv.org/abs/1810.01553 Biased Locking for Reader-Writer Locks

class
#ifdef PTHREAD_RWLOCK_INITIALIZER
    // if a dynamic initializer is required we cannot be sure that the type is trivially moveable (ie destruction can be skipped)
    // (on OSX deadlock was observed when trying to use a single statically
    // initialized pthread_rwlock_t instance as an initializer for the
    // rw_mutex::lock_ member)
    [[ clang::trivial_abi ]]
#endif
rw_mutex
{
public:
    rw_mutex() noexcept : rw_mutex( writer_preferring ) {}
   ~rw_mutex() noexcept { BOOST_VERIFY( pthread_rwlock_destroy( &lock_ ) == 0 ); }

    explicit // allow copy so as to enable use of compiler generated constructors/functions for types that contain rw_mutex members
    rw_mutex( [[ maybe_unused ]] rw_mutex const &  other ) noexcept : rw_mutex{} { BOOST_ASSERT_MSG( !other.is_locked(), "Copy allowed only for dormant mutexes" ); }
    rw_mutex( [[ maybe_unused ]] rw_mutex       && other ) noexcept : rw_mutex{} { BOOST_ASSERT_MSG( !other.is_locked(), "Relocation allowed only for dormant mutexes" ); }

    rw_mutex & operator=( [[ maybe_unused ]] rw_mutex && other ) noexcept
    {
        BOOST_ASSERT_MSG( !is_locked() && !other.is_locked(), "Relocation allowed only for dormant mutexes" );
        return *this;
    }

    // Read side, instrumented (debug only) to catch recursive read-acquisition -- a
    // documented hang on this non-recursive primitive (see read_recursion_registry.hpp).
    // The raw os_acquire_ro/os_release_ro below are the un-instrumented OS calls, used
    // by the reentrant rrw_mutex (which legitimately recurses and tracks its own depth)
    // and by reader_preferring_rw_mutex below (where nested reads are natively safe, so
    // this tripwire would be a false positive -- it shadows acquire_ro/release_ro to
    // skip straight to these raw calls instead).
    void acquire_ro() noexcept { detail::on_ro_acquire( this ); os_acquire_ro(); }
    // Registry first: a release without a matching acquire then fails the debug assert
    // before reaching the OS unlock (which would already be UB at that point).
    void release_ro() noexcept { detail::on_ro_release( this ); os_release_ro(); }

    void acquire_rw() noexcept { BOOST_VERIFY( pthread_rwlock_wrlock( &lock_ ) == 0 ); }
    void release_rw() noexcept { os_release_ro(); } // raw: a write hold is not read-tracked

    bool try_acquire_ro() noexcept
    {
        if ( !os_try_acquire_ro() )
        {
            return false;
        }
        detail::on_ro_acquire( this );
        return true;
    }
    bool try_acquire_rw() noexcept { return pthread_rwlock_trywrlock( &lock_ ) == 0; }

protected:
    // Tag-dispatched: selects the underlying rwlockattr kind once, at construction --
    // not a per-call branch, so the two preferences cost nothing extra per acquire/
    // release, and the RIGHT overload is picked by the compiler, not a runtime
    // comparison (see rw_preference.hpp). Protected: ordinary rw_mutex users always
    // get writer_preferring via the public default ctor above; only a derived class
    // (reader_preferring_rw_mutex below) picks the other policy. Both preferences are
    // the SAME type (rw_mutex) either way -- they only differ in what the chosen ctor
    // sets up, not in any method's signature or behaviour, so callers, guards
    // (rw_lock, ro_lock) and std::shared_lock all work identically regardless of which
    // preference a given instance was built with.
    explicit rw_mutex( writer_preferring_t ) noexcept;
#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
    // Only declared where the underlying primitive can actually be steered this way --
    // reader_preferring_rw_mutex (below) is itself only defined under the same macro,
    // so attempting to build a reader-preferring rw_mutex where this overload doesn't
    // exist is a compile error at the CALL site, not something this class needs to
    // runtime-check.
    explicit rw_mutex( reader_preferring_t ) noexcept;
#endif

    // Raw, un-instrumented OS read lock/unlock (pthread treats unlock uniformly for the
    // shared and exclusive sides). The reentrant rrw_mutex and reader_preferring_rw_mutex
    // (below) call these on the outermost acquire / last release (rrw_mutex) or always
    // (reader_preferring_rw_mutex, where every acquire is already safe) so the
    // non-recursive recursion assert above never fires for them.
    void os_acquire_ro() noexcept { BOOST_VERIFY( pthread_rwlock_rdlock   ( &lock_ ) == 0 ); }
    void os_release_ro() noexcept { BOOST_VERIFY( pthread_rwlock_unlock   ( &lock_ ) == 0 ); }
    bool os_try_acquire_ro() noexcept { return pthread_rwlock_tryrdlock( &lock_ ) == 0; }

    // Uniform hook for the derived reentrant rrw_mutex (see the Windows backend, which
    // detects lock acquisition while this thread holds the exclusive side). pthread does
    // not expose the write owner, and pthread_rwlock_rdlock itself returns EDEADLK for
    // that case on conforming implementations, so this backend has nothing extra to check.
    void verify_deadlock() const noexcept {}
public:

    // debugging aid
    bool is_locked() const noexcept
    {
        auto & mtbl{ const_cast<rw_mutex &>( *this ) };
        if ( mtbl.try_lock() ) // covers RO and RW locks
        {
            mtbl.unlock();
            return false;
        }
        return true;
    }

public: // std::shared_lock interface
    void   lock() noexcept { acquire_rw(); }
    void unlock() noexcept { release_rw(); }

    bool try_lock() noexcept { return try_acquire_rw(); }


    void   lock_shared() noexcept { acquire_ro(); }
    void unlock_shared() noexcept { release_ro(); }

    bool try_lock_shared() noexcept { return try_acquire_ro(); }

private:
    pthread_rwlock_t lock_; // this is yuge on OSX (200 bytes)
}; // class rw_mutex

inline rw_mutex::rw_mutex( writer_preferring_t ) noexcept
{
#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
    lock_ = PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;
#elifdef PTHREAD_RWLOCK_INITIALIZER
    // Best-effort fallback: no NP kind-setting API here, so this is just whatever the
    // OS default is -- not a guarantee of writer preference off Linux (see rrw_mutex.hpp).
    lock_ = PTHREAD_RWLOCK_INITIALIZER;
#else
    BOOST_VERIFY( pthread_rwlock_init( &lock_, nullptr ) == 0 );
#endif
}

#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
inline rw_mutex::rw_mutex( reader_preferring_t ) noexcept
{
    // No pthread_rwlockattr_t/setkind_np/destroy ceremony needed: glibc's own default
    // rwlock kind, PTHREAD_RWLOCK_DEFAULT_NP, IS PTHREAD_RWLOCK_PREFER_READER_NP
    // (pthread.h: "PTHREAD_RWLOCK_DEFAULT_NP = PTHREAD_RWLOCK_PREFER_READER_NP"), and
    // PTHREAD_RWLOCK_INITIALIZER bakes that default straight into the static struct --
    // so it already IS a reader-preferring initializer, at zero runtime cost.
    // PTHREAD_RWLOCK_INITIALIZER is guaranteed available wherever this overload is
    // (both live inside the same "__USE_UNIX98 || __USE_XOPEN2K" #if in pthread.h,
    // with the NP writer-nonrecursive macro additionally nested under __USE_GNU).
    lock_ = PTHREAD_RWLOCK_INITIALIZER;
}
#endif

// reader_preferring_rw_mutex: only defined where glibc's NP rwlock-kind extensions are
// available (Linux; neither macOS's pthread_rwlock nor Windows SRWLOCK offer a kind-
// setting API -- see rrw_mutex.hpp for what "reader preferring" means on those
// platforms instead). A thread that already holds the read side can safely re-acquire
// it directly on this type: reader preference makes nested reads deadlock-free
// natively, so this needs none of rrw_mutex's per-thread hold-tracking.
//
// Derives from (does not template) rw_mutex: acquire_rw/release_rw/try_acquire_rw and
// the write side of the std::shared_lock interface are inherited completely unchanged
// (the write side is non-recursive under either preference), and rw_lock (rw_mutex.hpp)
// -- a plain, non-templated RAII write guard -- accepts this type by ordinary base-
// reference binding, exactly as it already does for rrw_mutex.
#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
class
#ifdef PTHREAD_RWLOCK_INITIALIZER
    [[ clang::trivial_abi ]] // matches the base: no new data members, so trivial relocation is preserved
#endif
reader_preferring_rw_mutex : public rw_mutex
{
public:
    reader_preferring_rw_mutex() noexcept : rw_mutex( reader_preferring ) {}

    // Shadow (non-virtual -- rw_mutex has no virtuals) the read side only: reader
    // preference makes a nested read acquire natively deadlock-free (a queued writer
    // never blocks an already-arrived reader from re-acquiring), so the base class's
    // debug recursion tripwire -- a documented hang ONLY under writer preference, see
    // read_recursion_registry.hpp -- does not apply here and would be a false positive;
    // skip straight to the raw OS calls instead, same as rrw_mutex's outermost
    // acquire / last release (rrw_mutex.hpp).
    void acquire_ro() noexcept { verify_deadlock(); os_acquire_ro(); }
    void release_ro() noexcept { os_release_ro(); }
    bool try_acquire_ro() noexcept { verify_deadlock(); return os_try_acquire_ro(); }

    // std::shared_lock interface: re-route through the shadowed overrides above (the
    // base's non-virtual lock_shared()/unlock_shared()/try_lock_shared() would
    // statically bind to the base's acquire_ro/release_ro/try_acquire_ro -- the same
    // reason rrw_mutex re-declares these, see rrw_mutex.hpp).
    void   lock_shared() noexcept { acquire_ro(); }
    void unlock_shared() noexcept { release_ro(); }
    bool try_lock_shared() noexcept { return try_acquire_ro(); }
}; // class reader_preferring_rw_mutex
#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
