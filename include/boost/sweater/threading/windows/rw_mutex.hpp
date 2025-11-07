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
#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#include <windows.h>
//------------------------------------------------------------------------------
namespace boost::thrd_lite
{
//------------------------------------------------------------------------------

class [[ clang::trivial_abi ]] rw_mutex
{
public:
    constexpr rw_mutex(                   ) noexcept : lock_( SRWLOCK_INIT ) {}
    constexpr rw_mutex( rw_mutex && other ) noexcept : rw_mutex{} { BOOST_ASSUME( !other.locked() ); }
              rw_mutex( rw_mutex const &  ) = delete ;
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

    void acquire_ro() noexcept { verify_deadlock(); ::AcquireSRWLockShared( &lock_ ); }
    void release_ro() noexcept {                    ::ReleaseSRWLockShared( &lock_ ); }

    void acquire_rw() noexcept { verify_deadlock(); ::AcquireSRWLockExclusive( &lock_ ); BOOST_ASSERT( active_writer_ = ::GetCurrentThreadId() ); }
    void release_rw() noexcept {                    ::ReleaseSRWLockExclusive( &lock_ ); BOOST_ASSERT( !( active_writer_ = 0 ) ); }

    bool try_acquire_ro() noexcept { verify_deadlock(); return ::TryAcquireSRWLockShared   ( &lock_ ) != false; }
    bool try_acquire_rw() noexcept { verify_deadlock(); return ::TryAcquireSRWLockExclusive( &lock_ ) != false; }

    [[ gnu::pure ]] bool locked() const noexcept { return lock_.Ptr != nullptr; }

public: // std::shared_lock interface
    void   lock() noexcept { acquire_rw(); }
    void unlock() noexcept { release_rw(); }

    bool try_lock() noexcept { return try_acquire_rw(); }


    void   lock_shared() noexcept { acquire_ro(); }
    void unlock_shared() noexcept { release_ro(); }

    bool try_lock_shared() noexcept { return try_acquire_ro(); }

private:
    void verify_deadlock() const noexcept {
        BOOST_ASSERT( !locked() || ( active_writer_ != ::GetCurrentThreadId() ) );
    }
    // https://stackoverflow.com/questions/13206414/why-slim-reader-writer-exclusive-lock-outperforms-the-shared-lock/13216189#13216189
    // https://news.ycombinator.com/item?id=39581664 Bug in reader/writer locks in Windows API
    ::SRWLOCK lock_;
#ifndef NDEBUG
    DWORD active_writer_{};
#endif
}; // class rw_mutex

//------------------------------------------------------------------------------
} // namespace boost::thrd_lite
//------------------------------------------------------------------------------
