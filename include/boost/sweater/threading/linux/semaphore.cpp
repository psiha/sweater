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
#include "semaphore.hpp"

#include "../cpp/spin_lock.hpp" // only for nops()

#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#if defined( __EMSCRIPTEN__ ) // TODO: extract and deduplicate this implementation
#include <cmath>

#include <emscripten/threading.h>
#else
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif // Emscripten / Linux
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

namespace
{
#if defined( __EMSCRIPTEN__ )

	auto futex_wait( void * const addr, int const val ) noexcept { return emscripten_futex_wait( reinterpret_cast<volatile void *>( addr ), val, INFINITY ); }
	auto futex_wake( void * const addr, int const val ) noexcept { return emscripten_futex_wake( reinterpret_cast<volatile void *>( addr ), val           ); }

#elif defined( __linux__ )

    void futex( void * const addr1, int const op, int const val1 ) noexcept
    {
        ::syscall( SYS_futex, addr1, op | FUTEX_PRIVATE_FLAG, val1, nullptr, nullptr, 0 );
    }

    auto futex_wait( void * const addr, int const val ) noexcept { return futex( addr, FUTEX_WAIT, val ); }
	auto futex_wake( void * const addr, int const val ) noexcept { return futex( addr, FUTEX_WAKE, val ); }

#endif
} // anonymous namespace

#ifndef NDEBUG
futex_semaphore::~futex_semaphore() noexcept
{
#if 0 // need not hold on early destruction (when workers exit before waiting)
    BOOST_ASSUME( value_   == 0 );
#endif
    BOOST_ASSERT( waiters_ == 0 );
}
#endif // !NDEBUG

void futex_semaphore::signal( hardware_concurrency_t const count /*= 1*/ ) noexcept
{
#if BOOST_SWEATER_EXACT_WORKER_SELECTION && !defined( __ANDROID__ )
    BOOST_ASSUME( count == 1 );
#endif // BOOST_SWEATER_EXACT_WORKER_SELECTION
    if ( BOOST_UNLIKELY( !count ) )
        return;

    auto value{ value_.load( std::memory_order_relaxed ) };
    hardware_concurrency_t desired;
    do
    {
        desired = value + count + ( value < state::locked );
    } while ( !value_.compare_exchange_weak( value, desired, std::memory_order_acquire, std::memory_order_relaxed ) );

    if ( waiters_.load( std::memory_order_acquire ) )
    {
#   if 0 // FUTEX_WAKE implicitly performs this clipping
        auto const to_wake{ std::min({ static_cast<hardware_concurrency_t>( -old_value ), count, waiters_.load( std::memory_order_relaxed ) }) };
#   else
        auto const to_wake{ count };
#   endif
        futex_wake( &value_, to_wake );
    }
}

void futex_semaphore::wait() noexcept
{
    for ( ; ; )
    {
        auto value{ value_.load( std::memory_order_relaxed ) };
        while ( value > state::locked )
        {
            if ( try_decrement( value ) )
                return;
        }

        detail::overflow_checked_inc( waiters_ );
        value = state::locked; try_decrement( value );
        futex_wait( &value_, state::contested );
        detail::underflow_checked_dec( waiters_ );
    }
}

void futex_semaphore::wait( std::uint32_t const spin_count ) noexcept
{
    // waiting for atomic_ref
    auto value{ value_.load( std::memory_order_acquire ) };
    for ( auto spin_try{ 0U }; spin_try < spin_count; )
    {
        if ( value > state::locked )
        {
            if ( BOOST_LIKELY( try_decrement( value ) ) )
                return;
            BOOST_ASSUME( value > state::locked );
        }
        else
        {
            nops( 8 );
            value = value_.load( std::memory_order_acquire );
            ++spin_try;
        }
    }
    // value could be > locked here (in case the change happened on the last try)

    wait();
}

bool futex_semaphore::try_decrement( std::int32_t & __restrict last_value ) noexcept
{
    return BOOST_LIKELY( value_.compare_exchange_weak( last_value, last_value - 1, std::memory_order_acquire, std::memory_order_relaxed ) );
}

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
