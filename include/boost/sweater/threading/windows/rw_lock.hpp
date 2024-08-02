////////////////////////////////////////////////////////////////////////////////
///
/// \file rw_lock.hpp
/// -----------------
///
/// (c) Copyright Domagoj Saric 2024.
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
#include <windows.h>
//------------------------------------------------------------------------------
namespace boost::thrd_lite
{
//------------------------------------------------------------------------------

class rw_lock
{
public:
    constexpr rw_lock(                  ) noexcept : lock_( SRWLOCK_INIT ) {}
    constexpr rw_lock( rw_lock && other ) noexcept : lock_{ other.lock_ } { other.lock_ = SRWLOCK_INIT; }
              rw_lock( rw_lock const &  ) = delete ;
             ~rw_lock(                  ) = default;

    void acquire_ro() noexcept { ::AcquireSRWLockShared( &lock_ ); }
    void release_ro() noexcept { ::ReleaseSRWLockShared( &lock_ ); }

    void acquire_rw() noexcept { ::AcquireSRWLockExclusive( &lock_ ); }
    void release_rw() noexcept { ::ReleaseSRWLockExclusive( &lock_ ); }

    bool try_acquire_ro() noexcept { return ::TryAcquireSRWLockShared   ( &lock_ ) != false; }
    bool try_acquire_rw() noexcept { return ::TryAcquireSRWLockExclusive( &lock_ ) != false; }

public: // std::shared_lock interface
    void   lock() noexcept { acquire_rw(); }
    void unlock() noexcept { release_rw(); }

    bool try_lock() noexcept { return try_acquire_rw(); }


    void   lock_shared() noexcept { acquire_ro(); }
    void unlock_shared() noexcept { release_ro(); }

    bool try_lock_shared() noexcept { return try_acquire_ro(); }

private:
    ::SRWLOCK lock_;
}; // class rw_lock

//------------------------------------------------------------------------------
} // namespace boost::thrd_lite
//------------------------------------------------------------------------------
