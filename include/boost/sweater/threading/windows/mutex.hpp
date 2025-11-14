////////////////////////////////////////////////////////////////////////////////
///
/// \file mutex.hpp
/// ---------------
///
/// (c) Copyright Domagoj Saric 2016 - 2026.
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

class win32_condition_variable;

class win32_slim_mutex
{
public:
    win32_slim_mutex() = default;
   ~win32_slim_mutex() = default;
    explicit // allow copy so as to enable use compiler generated constructors/functions for types that contain mutex members
    win32_slim_mutex( [[ maybe_unused ]] win32_slim_mutex const &  other ) noexcept { BOOST_VERIFY_MSG( !other.lock_.Ptr, "Copy allowed only for dormant mutexes" ); }
    win32_slim_mutex( [[ maybe_unused ]] win32_slim_mutex       && other ) noexcept { BOOST_VERIFY_MSG( !other.lock_.Ptr, "Relocation allowed only for dormant mutexes" ); }

    void   lock() noexcept { ::AcquireSRWLockExclusive( &lock_ ); }
    void unlock() noexcept { ::ReleaseSRWLockExclusive( &lock_ ); }

    bool try_lock() noexcept { return ::TryAcquireSRWLockExclusive( &lock_ ) != false; }

private: friend class win32_condition_variable;
    ::SRWLOCK lock_ = SRWLOCK_INIT;
}; // class win32_slim_mutex

//------------------------------------------------------------------------------
} // namespace boost::thrd_lite
//------------------------------------------------------------------------------
