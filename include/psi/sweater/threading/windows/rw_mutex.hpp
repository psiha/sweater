////////////////////////////////////////////////////////////////////////////////
///
/// \file rw_mutex.hpp
/// ------------------
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
#include <psi/sweater/threading/read_recursion_registry.hpp>

#include <boost/assert.hpp>
#include <boost/config_ex.hpp> // BOOST_ASSUME

#include <windows.h>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

class [[ clang::trivial_abi ]] rw_mutex
{
public:
    constexpr rw_mutex() noexcept = default;
    explicit // allow copy so as to enable use compiler generated constructors/functions for types that contain mutex members
    constexpr rw_mutex( rw_mutex const &  other ) noexcept { BOOST_ASSUME( !other.locked() ); }
    constexpr rw_mutex( rw_mutex       && other ) noexcept { BOOST_ASSUME( !other.locked() ); }
#ifndef NDEBUG
    ~rw_mutex() { BOOST_ASSERT( !locked() ); }
#endif

    rw_mutex & operator=( rw_mutex && other ) noexcept
    {
        // this dummy operation makes sense only for dormant mutexes
        BOOST_ASSUME( !this->locked() );
        BOOST_ASSUME( !other.locked() );
        return *this;
    }

    // Read side, instrumented (debug only): verify_deadlock() catches read-while-holding
    // the exclusive lock, on_ro_acquire() catches recursive read-acquisition -- both are
    // documented hangs on this non-recursive primitive (see read_recursion_registry.hpp).
    // The raw os_acquire_ro/os_release_ro below are the un-instrumented OS calls, used by
    // the reentrant rrw_mutex (which legitimately recurses and tracks its own depth).
    void acquire_ro() noexcept { verify_deadlock(); detail::on_ro_acquire( this ); os_acquire_ro(); }
    // Registry first: a release without a matching acquire then fails the debug assert
    // before reaching the OS unlock (which would already be UB at that point).
    void release_ro() noexcept { detail::on_ro_release( this ); os_release_ro(); }

    void acquire_rw() noexcept { verify_deadlock(); ::AcquireSRWLockExclusive( &lock_ ); BOOST_ASSERT( active_writer_ = ::GetCurrentThreadId() ); }
    void release_rw() noexcept {                    ::ReleaseSRWLockExclusive( &lock_ ); BOOST_ASSERT( !( active_writer_ = 0 ) ); }

    bool try_acquire_ro() noexcept
    {
        verify_deadlock();
        if ( !os_try_acquire_ro() )
        {
            return false;
        }
        detail::on_ro_acquire( this );
        return true;
    }
    bool try_acquire_rw() noexcept { verify_deadlock(); return ::TryAcquireSRWLockExclusive( &lock_ ) != false; }

protected:
    // Raw, un-instrumented OS shared lock/unlock. The reentrant rrw_mutex calls these on
    // the outermost acquire / last release so its own recursion bookkeeping is the single
    // source of truth (and the non-recursive recursion assert above never fires for it).
    void os_acquire_ro() noexcept { ::AcquireSRWLockShared( &lock_ ); }
    void os_release_ro() noexcept { ::ReleaseSRWLockShared( &lock_ ); }
    bool os_try_acquire_ro() noexcept { return ::TryAcquireSRWLockShared( &lock_ ) != false; }

    // Debug check for taking any lock while this thread already holds the exclusive side
    // (a documented SRWLOCK deadlock). Protected so the derived reentrant rrw_mutex can
    // run the same check on its read path.
    void verify_deadlock() const noexcept {
        BOOST_ASSERT( !locked() || ( active_writer_ != ::GetCurrentThreadId() ) );
    }
public:

    [[ gnu::pure ]] bool locked() const noexcept { return lock_.Ptr != nullptr; }

public: // std::shared_lock interface
    void   lock() noexcept { acquire_rw(); }
    void unlock() noexcept { release_rw(); }

    bool try_lock() noexcept { return try_acquire_rw(); }


    void   lock_shared() noexcept { acquire_ro(); }
    void unlock_shared() noexcept { release_ro(); }

    bool try_lock_shared() noexcept { return try_acquire_ro(); }

private:
    // https://stackoverflow.com/questions/13206414/why-slim-reader-writer-exclusive-lock-outperforms-the-shared-lock/13216189#13216189
    // https://news.ycombinator.com/item?id=39581664 Bug in reader/writer locks in Windows API
    ::SRWLOCK lock_ = SRWLOCK_INIT;
#ifndef NDEBUG
    DWORD active_writer_{};
#endif
}; // class rw_mutex

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
