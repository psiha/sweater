////////////////////////////////////////////////////////////////////////////////
///
/// \file barrier.hpp
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
#include "../impls/generic_config.hpp" //...mrmlj...spaghetti...

#include "condvar.hpp"
#include "hardware_concurrency.hpp"
#include "mutex.hpp"
#include "thread.hpp"

#if BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
#include "cpp/spin_lock.hpp"
#endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION

#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#include <mutex>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater::generic::events { void caller_stalled( std::uint8_t current_boost ) noexcept; } //...mrmlj...spaghetti...
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

class barrier
{
public:
    barrier() noexcept : barrier( 0 ) {}
    barrier( hardware_concurrency_t const initial_value ) noexcept : counter_{ initial_value } {}
#ifndef NDEBUG
   ~barrier() noexcept { BOOST_ASSERT( counter_ == 0 ); }
#endif // NDEBUG

    void initialize( hardware_concurrency_t const initial_value ) noexcept
    {
        BOOST_ASSERT_MSG( counter_ == 0, "Already initialized" );
        counter_.store( initial_value, std::memory_order_release );
    }

    void add_expected_arrival() noexcept { counter_.fetch_add( 1, std::memory_order_acquire ); }

    auto actives() const noexcept { return counter_.load( std::memory_order_acquire ); }

#if BOOST_SWEATER_USE_CALLER_THREAD
    void use_spin_wait( bool const value ) noexcept { spin_wait_ = value; }
#endif // BOOST_SWEATER_USE_CALLER_THREAD

    void arrive() noexcept
    {
        BOOST_ASSERT( counter_ > 0 );
#   if BOOST_SWEATER_USE_CALLER_THREAD
        if ( BOOST_LIKELY( spin_wait_ ) )
        {
            BOOST_VERIFY( counter_.fetch_sub( 1, std::memory_order_release ) >= 1 );
            return;
        }
#   endif // BOOST_SWEATER_USE_CALLER_THREAD
        bool everyone_arrived;
        {
            std::scoped_lock<mutex> lock{ mutex_ };
            BOOST_ASSERT( counter_ > 0 );
            everyone_arrived = ( counter_.fetch_sub( 1, std::memory_order_relaxed ) == 1 );
        }
        if ( BOOST_UNLIKELY( everyone_arrived ) )
            event_.notify_one();
    }

    void wait() noexcept
    {
#   if BOOST_SWEATER_USE_CALLER_THREAD
        BOOST_ASSERT( !spin_wait_ );
#   endif // BOOST_SWEATER_USE_CALLER_THREAD
        std::scoped_lock<mutex> lock{ mutex_ };
        while ( BOOST_UNLIKELY( counter_.load( std::memory_order_relaxed ) != 0 ) )
            event_.wait( mutex_ );
    }

#if BOOST_SWEATER_USE_CALLER_THREAD
    BOOST_NOINLINE void spin_wait
    (
#if  BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
        std::uint32_t const spin_count,
#endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
#if  BOOST_SWEATER_CALLER_BOOST //...mrmlj...spaghetti...
        std::uint8_t & caller_boost, std::uint8_t const caller_boost_max
#else
        std::uint8_t const caller_boost = 0
#endif // BOOST_SWEATER_CALLER_BOOST
    ) noexcept
#ifdef __clang__
    __attribute__(( no_sanitize( "unsigned-integer-overflow" ) ))
#endif
    {
        BOOST_ASSERT( spin_wait_ );

#   if BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
        auto spin_tries{ spin_count };
        while ( spin_tries-- )
        {
#   endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
            if ( BOOST_LIKELY( actives() == 0 ) )
            {
#           if BOOST_SWEATER_CALLER_BOOST
                caller_boost = std::max<std::int8_t>( 0, static_cast<std::int8_t>( caller_boost ) - 1 );
#           endif // BOOST_SWEATER_CALLER_BOOST
                return;
            }
#   if BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
            nops( 8 );
        }
#   endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION

        sweater::generic::events::caller_stalled( caller_boost );
#   if BOOST_SWEATER_CALLER_BOOST
        caller_boost = std::min<std::uint8_t>( caller_boost_max, caller_boost + 1 );
#   endif // BOOST_SWEATER_CALLER_BOOST
        while ( BOOST_UNLIKELY( counter_.load( std::memory_order_acquire ) != 0 ) )
        {
            std::this_thread::yield();
        }
    }
#endif // BOOST_SWEATER_USE_CALLER_THREAD

private:
    // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2007/n2406.html#gen_cond_var
    using worker_counter = std::atomic<hardware_concurrency_t>;

    worker_counter     counter_;
#if BOOST_SWEATER_USE_CALLER_THREAD
    bool               spin_wait_{ false };
#endif // BOOST_SWEATER_USE_CALLER_THREAD
    mutex              mutex_  ;
    condition_variable event_  ;
}; // class barrier

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
