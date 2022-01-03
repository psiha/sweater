////////////////////////////////////////////////////////////////////////////////
///
/// \file condvar.hpp
/// -----------------
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
#include "mutex.hpp"

#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#include <mutex>

#include <windows.h>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

// Strategies for Implementing POSIX Condition Variables on Win32 http://www.cs.wustl.edu/~schmidt/win32-cv-1.html
// http://developers.slashdot.org/story/07/02/26/1211220/pthreads-vs-win32-threads
// http://nasutechtips.blogspot.com/2010/11/slim-read-write-srw-locks.html

class win32_condition_variable
{
public:
    using mutex_t = win32_slim_mutex;
    using lock_t  = std::unique_lock<mutex_t>;

    win32_condition_variable(                                   ) noexcept : cv_( CONDITION_VARIABLE_INIT ) {}
    win32_condition_variable( win32_condition_variable && other ) noexcept : cv_{ other.cv_ } { other.cv_ = CONDITION_VARIABLE_INIT; }
    win32_condition_variable( win32_condition_variable const &  ) = delete ;
   ~win32_condition_variable(                                   ) = default;

    void notify_all() noexcept { ::WakeAllConditionVariable( &cv_ ); }
    void notify_one() noexcept { ::WakeConditionVariable   ( &cv_ ); }

    void wait( lock_t  & lock ) noexcept { BOOST_VERIFY( wait( lock, INFINITE ) ); }
    void wait( mutex_t & m    ) noexcept { BOOST_VERIFY( wait( m   , INFINITE ) ); }

    bool wait( lock_t  & lock                , std::uint32_t const milliseconds ) noexcept { return wait( *lock.mutex(), milliseconds ); }
    bool wait( mutex_t & m /*must be locked*/, std::uint32_t const milliseconds ) noexcept
    {
        auto const result( ::SleepConditionVariableSRW( &cv_, &m.lock_, milliseconds, 0/*CONDITION_VARIABLE_LOCKMODE_SHARED*/ ) );
        BOOST_ASSERT( result || ::GetLastError() == ERROR_TIMEOUT );
        return result != false;
    }

private:
    ::CONDITION_VARIABLE cv_;
}; // class win32_condition_variable

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
