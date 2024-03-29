////////////////////////////////////////////////////////////////////////////////
///
/// \file generic_barrier.cpp
/// -------------------------
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
#include "barrier.hpp"

#include "condvar.hpp"
#include "hardware_concurrency.hpp"
#include "mutex.hpp"
#include "thread.hpp"

#include "cpp/spin_lock.hpp" // only for nops

#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#include <limits>
#include <mutex>
#include <thread>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

generic_barrier::generic_barrier() noexcept : generic_barrier( 0 ) {}
generic_barrier::generic_barrier( hardware_concurrency_t const initial_value ) noexcept : counter_{ initial_value } {}
#ifndef NDEBUG
generic_barrier::~generic_barrier() noexcept { BOOST_ASSERT( counter_ == 0 ); }
#endif // NDEBUG

void generic_barrier::initialize( hardware_concurrency_t const initial_value ) noexcept
{
    BOOST_ASSERT_MSG( counter_ == 0, "Already initialized" );
    counter_.store( initial_value, std::memory_order_release );
}

void generic_barrier::add_expected_arrival() noexcept { detail::overflow_checked_add( counter_, worker_counter::value_type{ 1 } ); }

hardware_concurrency_t generic_barrier::actives         () const noexcept { return counter_.load( std::memory_order_acquire ); }
bool                   generic_barrier::everyone_arrived() const noexcept { return actives() == 0; }

#if BOOST_SWEATER_USE_CALLER_THREAD
void generic_barrier::use_spin_wait( bool const value ) noexcept { spin_wait_ = value; }
#endif // BOOST_SWEATER_USE_CALLER_THREAD

void generic_barrier::arrive() noexcept
{
    BOOST_ASSERT( counter_ > 0 );
#if BOOST_SWEATER_USE_CALLER_THREAD
    if ( BOOST_LIKELY( spin_wait_ ) )
    {
        detail::underflow_checked_dec( counter_ );
        return;
    }
#endif // BOOST_SWEATER_USE_CALLER_THREAD
    // Here we have to perform the notification while holding the lock to
    // prevent event_ getting used after this gets destroyed (goes out of scope
    // in the waiting thread immediately after counter_ reaches zero).
    BOOST_ASSERT( counter_ > 0 );
    std::scoped_lock<mutex> lock{ mutex_ };
    bool const everyone_arrived{ counter_.fetch_sub( 1, std::memory_order_relaxed ) == 1 };
    if ( BOOST_UNLIKELY( everyone_arrived ) )
        event_.notify_one();
}

void generic_barrier::wait() noexcept
{
#if BOOST_SWEATER_USE_CALLER_THREAD
    BOOST_ASSUME( !spin_wait_ );
#endif // BOOST_SWEATER_USE_CALLER_THREAD
    std::scoped_lock<mutex> lock{ mutex_ };
    while ( BOOST_UNLIKELY( counter_.load( std::memory_order_relaxed ) != 0 ) )
        event_.wait( mutex_ );
}

#if BOOST_SWEATER_USE_CALLER_THREAD
bool generic_barrier::spin_wait( std::uint32_t const nop_spin_count ) noexcept
#ifdef __clang__
__attribute__(( no_sanitize( "unsigned-integer-overflow" ) ))
#endif
{
    BOOST_ASSUME( spin_wait_ );

    auto spin_tries{ nop_spin_count };
    while ( spin_tries-- )
    {
        if ( BOOST_LIKELY( everyone_arrived() ) )
            return ( nop_spin_count - spin_tries ) < ( nop_spin_count / 8 );
        nops( 8 );
    }

    while ( BOOST_UNLIKELY( !everyone_arrived() ) )
    {
        std::this_thread::yield();
    }
    return true;
}
#endif // BOOST_SWEATER_USE_CALLER_THREAD

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
