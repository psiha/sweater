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

// basic_rw_mutex<Preference>: Preference is an NTTP, not a ctor argument, so the
// choice costs nothing per acquire/release -- it only selects which static
// initializer / rwlockattr the ctor below sets up once. writer_preferring is the
// default and is the pre-existing behaviour (unchanged). reader_preferring is only
// defined (see the alias below) where glibc's NP rwlock-kind extensions are
// available -- there is no portable POSIX API to request it otherwise, so
// instantiating it on a platform without those extensions must not silently
// degrade to "whatever the OS default is": see rw_mutex_traits::supports_reader_preference.
template <rw_preference Preference = rw_preference::writer_preferring>
class
#ifdef PTHREAD_RWLOCK_INITIALIZER
    // if a dynamic initializer is required we cannot be sure that the type is trivially moveable (ie destruction can be skipped)
    // (on OSX deadlock was observed when trying to use a single statically
    // initialized pthread_rwlock_t instance as an initializer for the
    // rw_mutex::lock_ member)
    [[ clang::trivial_abi ]]
#endif
basic_rw_mutex
{
public:
    basic_rw_mutex() noexcept;
   ~basic_rw_mutex() noexcept { BOOST_VERIFY( pthread_rwlock_destroy( &lock_ ) == 0 ); }

    explicit // allow copy so as to enable use of compiler generated constructors/functions for types that contain rw_mutex members
    basic_rw_mutex( [[ maybe_unused ]] basic_rw_mutex const &  other ) noexcept : basic_rw_mutex{} { BOOST_ASSERT_MSG( !other.is_locked(), "Copy allowed only for dormant mutexes" ); }
    basic_rw_mutex( [[ maybe_unused ]] basic_rw_mutex       && other ) noexcept : basic_rw_mutex{} { BOOST_ASSERT_MSG( !other.is_locked(), "Relocation allowed only for dormant mutexes" ); }

    basic_rw_mutex & operator=( [[ maybe_unused ]] basic_rw_mutex && other ) noexcept
    {
        BOOST_ASSERT_MSG( !is_locked() && !other.is_locked(), "Relocation allowed only for dormant mutexes" );
        return *this;
    }

    // Read side, instrumented (debug only) to catch recursive read-acquisition -- a
    // documented hang, but *only* under writer_preferring (see read_recursion_registry.hpp):
    // under reader_preferring a nested read acquire is never behind a queued writer, so
    // it is not a bug and the tripwire (and its registry bookkeeping) is skipped entirely
    // at compile time -- consistent with Preference being a zero-runtime-cost NTTP.
    // The raw os_acquire_ro/os_release_ro below are the un-instrumented OS calls, used
    // by the reentrant rrw_mutex (which legitimately recurses and tracks its own depth).
    void acquire_ro() noexcept
    {
        if constexpr ( Preference == rw_preference::writer_preferring ) { detail::on_ro_acquire( this ); }
        os_acquire_ro();
    }
    // Registry first: a release without a matching acquire then fails the debug assert
    // before reaching the OS unlock (which would already be UB at that point).
    void release_ro() noexcept
    {
        if constexpr ( Preference == rw_preference::writer_preferring ) { detail::on_ro_release( this ); }
        os_release_ro();
    }

    void acquire_rw() noexcept { BOOST_VERIFY( pthread_rwlock_wrlock( &lock_ ) == 0 ); }
    void release_rw() noexcept { os_release_ro(); } // raw: a write hold is not read-tracked

    bool try_acquire_ro() noexcept
    {
        if ( !os_try_acquire_ro() )
        {
            return false;
        }
        if constexpr ( Preference == rw_preference::writer_preferring ) { detail::on_ro_acquire( this ); }
        return true;
    }
    bool try_acquire_rw() noexcept { return pthread_rwlock_trywrlock( &lock_ ) == 0; }

protected:
    // Raw, un-instrumented OS read lock/unlock (pthread treats unlock uniformly for the
    // shared and exclusive sides). The reentrant rrw_mutex calls these on the outermost
    // acquire / last release so its own recursion bookkeeping is the single source of
    // truth (and the non-recursive recursion assert above never fires for it).
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
        auto & mtbl{ const_cast<basic_rw_mutex &>( *this ) };
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
}; // class basic_rw_mutex

template <rw_preference Preference>
inline basic_rw_mutex<Preference>::basic_rw_mutex() noexcept
{
    if constexpr ( Preference == rw_preference::reader_preferring )
    {
        // Only ever instantiated where the NP kind-setting API exists -- see the
        // reader_preferring_rw_mutex alias below, which is itself only defined under
        // the same #ifdef. No portable fallback: unlike writer_preferring (which can
        // degrade to "whatever the OS default is" via plain PTHREAD_RWLOCK_INITIALIZER
        // / pthread_rwlock_init(nullptr) below), silently degrading a *requested*
        // reader preference would defeat the entire point of exposing the choice.
#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
        pthread_rwlockattr_t attr;
        BOOST_VERIFY( pthread_rwlockattr_init( &attr ) == 0 );
        BOOST_VERIFY( pthread_rwlockattr_setkind_np( &attr, PTHREAD_RWLOCK_PREFER_READER_NP ) == 0 );
        BOOST_VERIFY( pthread_rwlock_init( &lock_, &attr ) == 0 );
        BOOST_VERIFY( pthread_rwlockattr_destroy( &attr ) == 0 );
#else
        // Guards against direct use of basic_rw_mutex<reader_preferring>, bypassing
        // the reader_preferring_rw_mutex alias that is gated on this same #ifdef --
        // fail to compile rather than leave lock_ default-initialized (UB).
        static_assert( Preference != rw_preference::reader_preferring,
            "reader_preferring rw_mutex requires glibc NP rwlock-kind extensions, "
            "unavailable on this platform -- use basic_rw_mutex<writer_preferring> "
            "(the `rw_mutex` alias) instead" );
#endif
    }
    else
    {
#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
        lock_ = PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;
#elifdef PTHREAD_RWLOCK_INITIALIZER
        lock_ = PTHREAD_RWLOCK_INITIALIZER;
#else
        BOOST_VERIFY( pthread_rwlock_init( &lock_, nullptr ) == 0 );
#endif
    }
}

// writer_preferring is the pre-existing, always-available behaviour (unchanged name
// and semantics: existing code using `rw_mutex` is unaffected by this header).
using rw_mutex = basic_rw_mutex<rw_preference::writer_preferring>;

// reader_preferring_rw_mutex: only defined where glibc's NP rwlock-kind extensions
// are available (Linux; not offered by macOS's pthread_rwlock, and not offered by
// Windows SRWLOCK -- see windows/rw_mutex.hpp). A thread that already holds the read
// side can safely re-acquire it directly on this type: reader preference makes
// nested reads deadlock-free natively, so a caller that does not also need writer-
// starvation avoidance can use this instead of rrw_mutex and skip its per-thread
// hold-tracking machinery entirely (see rrw_mutex.hpp).
#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
using reader_preferring_rw_mutex = basic_rw_mutex<rw_preference::reader_preferring>;
#endif

template <rw_preference Preference>
struct rw_mutex_traits<basic_rw_mutex<Preference>>
{
    static constexpr rw_preference preference = Preference;

    // "Guaranteed" here means: backed by an explicit glibc NP rwlock-kind request,
    // not an unspecified OS default that happens to often behave this way. Both are
    // only guaranteed where PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP exists
    // (see the ctor above and posix/rw_mutex.hpp's writer_preferring fallback chain,
    // which is best-effort -- not a guarantee -- off that platform).
#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
    static constexpr bool supports_reader_preference = true;
    static constexpr bool supports_writer_preference = true;
#else
    static constexpr bool supports_reader_preference = false;
    static constexpr bool supports_writer_preference = false;
#endif
};

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
