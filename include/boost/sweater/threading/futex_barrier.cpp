////////////////////////////////////////////////////////////////////////////////
///
/// \file futex_barrier.cpp
/// -----------------------
///
/// (c) Copyright Domagoj Saric 2021 - 2023.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "futex_barrier.hpp"

#include "cpp/spin_lock.hpp" //...mrmlj...check adders

#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#include <mutex>
#include <thread> // for yield
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

futex_barrier::futex_barrier() noexcept : futex_barrier( 0 ) {}
futex_barrier::futex_barrier( hardware_concurrency_t const initial_value ) noexcept : counter_{ initial_value } {}
#ifndef NDEBUG
futex_barrier::~futex_barrier() noexcept { BOOST_ASSERT( counter_ == 0 ); }
#endif
void futex_barrier::initialize( hardware_concurrency_t const initial_value ) noexcept
{
    BOOST_ASSERT_MSG( counter_ == 0, "Already initialized" );
    counter_.store( initial_value, std::memory_order_release );
}

void futex_barrier::add_expected_arrival() noexcept { detail::overflow_checked_add( counter_, futex::value_type{ 1 } ); }

hardware_concurrency_t futex_barrier::actives         () const noexcept { return static_cast<hardware_concurrency_t>( counter_.load( std::memory_order_acquire ) ); }
bool                   futex_barrier::everyone_arrived() const noexcept { return actives() == 0; }

#if BOOST_SWEATER_USE_CALLER_THREAD
void futex_barrier::use_spin_wait( bool const value ) noexcept { spin_wait_ = value; }
#endif // BOOST_SWEATER_USE_CALLER_THREAD

void futex_barrier::arrive() noexcept
{
    BOOST_ASSERT( counter_ > 0 );
#if BOOST_SWEATER_USE_CALLER_THREAD
    if ( BOOST_LIKELY( spin_wait_ ) )
    {
        detail::underflow_checked_dec( counter_ );
        return;
    }
#endif // BOOST_SWEATER_USE_CALLER_THREAD
    
    auto const everyone_arrived{ counter_.fetch_sub( 1, std::memory_order_release ) == 1 };
    // Possible race here if this gets accessed after it gets destroyed
    // (goes out of scope in the waiting thread after counter_ reaches zero) -
    // with futexes this is however harmless as the memory pointed to by this
    // is still valid (as in correctly mapped in the process - as a thread's
    // stack space or even, case not used by Sweater, freed heap memory).
    if ( BOOST_UNLIKELY( everyone_arrived ) )
    {
#   if 0
        // In case the above scenario happens this check can fail (counter_ can
        // contain garbage).
        BOOST_ASSERT( counter_ == 0 );
#   endif
        counter_.wake_one();
    }
}

void futex_barrier::wait() noexcept
{
    // http://locklessinc.com/articles/barriers
    // https://sourceware.org/git/?p=glibc.git;a=blob;f=nptl/pthread_barrier_wait.c
    // https://github.com/forhappy/barriers/blob/master/futex-barrier.c

#if BOOST_SWEATER_USE_CALLER_THREAD
    BOOST_ASSUME( !spin_wait_ );
#endif // BOOST_SWEATER_USE_CALLER_THREAD

    for ( ; ; )
    {
        auto const non_arrived_count{ counter_.load( std::memory_order_acquire ) };
        if ( non_arrived_count == 0 )
            return;
        counter_.wait_if_equal( non_arrived_count );
    }
}

#if BOOST_SWEATER_USE_CALLER_THREAD
bool futex_barrier::spin_wait( std::uint32_t const nop_spin_count ) noexcept
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
