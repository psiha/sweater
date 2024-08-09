////////////////////////////////////////////////////////////////////////////////
///
/// \file rw_mutex.hpp
/// ------------------
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
    constexpr rw_mutex( rw_mutex && other ) noexcept : lock_{ other.lock_ } { other.lock_ = SRWLOCK_INIT; }
              rw_mutex( rw_mutex const &  ) = delete ;
             ~rw_mutex(                   ) = default;

    rw_mutex & operator=( rw_mutex && other ) noexcept
    {
        // this dummy operation makes sense only for dormant mutexes
        BOOST_ASSUME( !this->locked() );
        BOOST_ASSUME( !other.locked() );
        return *this;
    }

    void acquire_ro() noexcept { ::AcquireSRWLockShared( &lock_ ); }
    void release_ro() noexcept { ::ReleaseSRWLockShared( &lock_ ); }

    void acquire_rw() noexcept { ::AcquireSRWLockExclusive( &lock_ ); }
    void release_rw() noexcept { ::ReleaseSRWLockExclusive( &lock_ ); }

    bool try_acquire_ro() noexcept { return ::TryAcquireSRWLockShared   ( &lock_ ) != false; }
    bool try_acquire_rw() noexcept { return ::TryAcquireSRWLockExclusive( &lock_ ) != false; }

    [[ gnu::pure ]] bool locked() const noexcept { return lock_.Ptr != nullptr; }

public: // std::shared_lock interface
    void   lock() noexcept { acquire_rw(); }
    void unlock() noexcept { release_rw(); }

    bool try_lock() noexcept { return try_acquire_rw(); }


    void   lock_shared() noexcept { acquire_ro(); }
    void unlock_shared() noexcept { release_ro(); }

    bool try_lock_shared() noexcept { return try_acquire_ro(); }

private:
    ::SRWLOCK lock_;
}; // class rw_mutex

//------------------------------------------------------------------------------
} // namespace boost::thrd_lite
//------------------------------------------------------------------------------
