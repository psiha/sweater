////////////////////////////////////////////////////////////////////////////////
///
/// \file mutex.hpp
/// ---------------
///
/// (c) Copyright Domagoj Saric 2016 - 2021.
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
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

class win32_condition_variable;

class win32_slim_mutex
{
public:
    win32_slim_mutex(                           ) noexcept : lock_( SRWLOCK_INIT ) {}
    win32_slim_mutex( win32_slim_mutex && other ) noexcept : lock_{ other.lock_ } { other.lock_ = SRWLOCK_INIT; }
    win32_slim_mutex( win32_slim_mutex const &  ) = delete ;
   ~win32_slim_mutex(                           ) = default;

    void   lock() noexcept { ::AcquireSRWLockExclusive( &lock_ ); }
    void unlock() noexcept { ::ReleaseSRWLockExclusive( &lock_ ); }

    bool try_lock() noexcept { return ::TryAcquireSRWLockExclusive( &lock_ ) != false; }

private: friend class win32_condition_variable;
    ::SRWLOCK lock_;
}; // class win32_slim_mutex

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
