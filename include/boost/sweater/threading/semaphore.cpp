////////////////////////////////////////////////////////////////////////////////
///
/// \file semaphore.cpp
/// -------------------
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
#if !defined( __linux__ ) && !defined( BOOST_HAS_PTHREADS )
#include "semaphore.hpp"

#include "cpp/spin_lock.hpp"
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

#ifndef NDEBUG
semaphore::~semaphore() noexcept
{
#if 0 // need not hold on early destruction (when workers exit before waiting)
    BOOST_ASSUME( value_   == 0 );
#endif
    BOOST_ASSUME( waiters_ == 0 );
}
#endif // !NDEBUG

void semaphore::signal( hardware_concurrency_t const count ) noexcept
{
#if BOOST_SWEATER_EXACT_WORKER_SELECTION
    BOOST_ASSUME( count == 1 );
#endif // BOOST_SWEATER_EXACT_WORKER_SELECTION
    auto const old_value{ value_.fetch_add( count, std::memory_order_release ) };
    if ( old_value > 0 )
    {
#   if 0 // for tiny work waiters_ can already increment/appear after the fetch_add
        BOOST_ASSUME( waiters_ == 0 );
#   endif // disabled
        return;
    }
#if BOOST_SWEATER_EXACT_WORKER_SELECTION
    BOOST_ASSUME( waiters_ <= 1 );
    {
        std::scoped_lock<mutex> lock{ mutex_ };
        ++to_release_;
        if ( !waiters_ ) // unknown whether condvar notify can avoid syscalls when there are no waiters
            return;
    }
    condition_.notify_one();
#else
    auto const to_wake{ std::min( static_cast<hardware_concurrency_t>( -old_value ), count ) };
    {
        std::scoped_lock<mutex> lock{ mutex_ };
        to_release_ += to_wake;
        if ( !waiters_ ) // unknown whether condvar notify can avoid syscalls when there are no waiters
            return;
    }
    if ( to_wake < waiters_ )
    {
        for ( auto notified{ 0U }; notified < to_wake; ++notified )
            condition_.notify_one();
    }
    else
    {
        condition_.notify_all();
    }
#endif // BOOST_SWEATER_EXACT_WORKER_SELECTION
}

void semaphore::wait( std::uint32_t const spin_count ) noexcept
{
    // waiting for atomic_ref
    auto value{ value_.load( std::memory_order_relaxed ) };
    for ( auto spin_try{ 0U }; spin_try < spin_count; ++spin_try )
    {
        if ( value > 0 )
        {
            if ( value_.compare_exchange_weak( value, value - 1, std::memory_order_acquire, std::memory_order_relaxed ) )
                return;
        }
        else
        {
            nops( 8 );
            value = value_.load( std::memory_order_relaxed );
        }

    }
    BOOST_ASSUME( value <= 0 );

    wait();
}

void semaphore::wait() noexcept
{
    auto const old_value{ value_.fetch_sub( 1, std::memory_order_acquire ) };
    if ( old_value > 0 )
        return;
    std::unique_lock<mutex> lock{ mutex_ };
    ++waiters_;
    while ( to_release_ == 0 ) // support spurious wakeups
        condition_.wait( lock );
    --to_release_;
    --waiters_;
}

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // !Linux && !POSIX
