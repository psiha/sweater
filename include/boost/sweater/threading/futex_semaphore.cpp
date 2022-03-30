////////////////////////////////////////////////////////////////////////////////
///
/// \file futex_semaphore.cpp
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
#include "semaphore.hpp"

#include "cpp/spin_lock.hpp" // only for nops()

#include <boost/assert.hpp>
#include <boost/config_ex.hpp>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

// Here we only use global semaphore objects so there is no need for the
// race-condition workaround described in the links below.
// http://git.musl-libc.org/cgit/musl/commit/?id=88c4e720317845a8e01aee03f142ba82674cd23d
// https://github.com/preshing/cpp11-on-multicore/blob/master/common/sema.h
// https://stackoverflow.com/questions/36094115/c-low-level-semaphore-implementation
// https://comp.programming.threads.narkive.com/IRKGW6HP/too-much-overhead-from-semaphores

#ifndef NDEBUG
semaphore::~semaphore() noexcept
{
#if 0 // need not hold on early destruction (when workers exit before waiting)
    BOOST_ASSUME( value_   == 0 );
#endif
    BOOST_ASSERT( waiters_ == 0 );
}
#endif // !NDEBUG

void semaphore::signal( hardware_concurrency_t const count /*= 1*/ ) noexcept
{
    // https://softwareengineering.stackexchange.com/questions/340284/mutex-vs-semaphore-how-to-implement-them-not-in-terms-of-the-other

#if BOOST_SWEATER_EXACT_WORKER_SELECTION && !defined( __ANDROID__ )
    BOOST_ASSUME( count == 1 );
#endif // BOOST_SWEATER_EXACT_WORKER_SELECTION
    if ( BOOST_UNLIKELY( !count ) )
        return;

    auto value{ load( std::memory_order_relaxed ) };
    futex::value_type desired;
    do
    {
        auto const is_contested{ value < state::locked };
        desired = value + count + is_contested;
    } while ( !value_.compare_exchange_weak( reinterpret_cast<futex::value_type &>( value ), desired, std::memory_order_acquire, std::memory_order_relaxed ) );

    if ( waiters_.load( std::memory_order_acquire ) )
    {
#   if 0 // FUTEX_WAKE implicitly performs this clipping
        auto const to_wake{ std::min({ static_cast<hardware_concurrency_t>( -old_value ), count, waiters_.load( std::memory_order_relaxed ) }) };
#   else
        auto const to_wake{ count };
#   endif
        value_.wake( to_wake );
    }
}

void semaphore::wait() noexcept
{
    for ( ; ; )
    {
        auto value{ load( std::memory_order_relaxed ) };
        while ( value > state::locked )
        {
            if ( try_decrement( value ) )
                return;
        }

        detail::overflow_checked_inc( waiters_ );
        value = state::locked; try_decrement( value );
        value_.wait_if_equal( static_cast< futex::value_type >( state::contested ) );
        detail::underflow_checked_dec( waiters_ );
    }
}

void semaphore::wait( std::uint32_t const spin_count ) noexcept
{
    // waiting for atomic_ref
    auto value{ load( std::memory_order_acquire ) };
    for ( auto spin_try{ 0U }; spin_try < spin_count; )
    {
        if ( value > state::locked )
        {
            if ( BOOST_LIKELY( try_decrement( value ) ) )
                return;
            //BOOST_ASSUME( value > state::locked ); ...mrmlj...failed on Meizu Pro 6...check...
        }
        else
        {
            nops( 8 );
            value = static_cast<signed_futex_value_t>( value_.load( std::memory_order_acquire ) );
            ++spin_try;
        }
    }
    // value could be > locked here (in case the change happened on the last try)

    wait();
}

semaphore::signed_futex_value_t semaphore::load( std::memory_order const memory_order ) const noexcept
{
    return static_cast<signed_futex_value_t>( value_.load( memory_order ) );
}

bool semaphore::try_decrement( signed_futex_value_t & __restrict last_value ) noexcept
{
    return BOOST_LIKELY
    (
        value_.compare_exchange_weak
        (
            reinterpret_cast<futex::value_type &>( last_value     ),
            static_cast     <futex::value_type  >( last_value - 1 ),
            std::memory_order_acquire,
            std::memory_order_relaxed
        )
    );
}

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
