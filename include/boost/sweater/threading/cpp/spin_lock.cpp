////////////////////////////////////////////////////////////////////////////////
///
/// \file spin_lock.cpp
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
#include "spin_lock.hpp"

#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#include <limits>

#ifdef _MSC_VER
#include <emmintrin.h>
#endif
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

namespace detail
{
    void overflow_checked_add( std::atomic<hardware_concurrency_t> & object, hardware_concurrency_t const value ) noexcept
    {
        BOOST_VERIFY( object.fetch_add( value, std::memory_order_acquire ) < std::numeric_limits<hardware_concurrency_t>::max() );
    }

    void overflow_checked_inc( std::atomic<hardware_concurrency_t> & object ) noexcept
    {
        overflow_checked_add( object, 1 );
    }

    void underflow_checked_dec( std::atomic<hardware_concurrency_t> & object ) noexcept
    {
        BOOST_VERIFY( object.fetch_sub( 1, std::memory_order_release ) > 0 );
    }
} // detail

BOOST_ATTRIBUTES( BOOST_COLD )
void nop() noexcept // 'minimally-hot' spin nop
{
    // TODO http://open-std.org/JTC1/SC22/WG21/docs/papers/2016/p0514r0.pdf
#if   defined( __i386__ ) || defined( __x86_64__ )
    asm volatile( "rep; nop" ::: "memory" );
#elif defined( __aarch64__ )
    asm volatile( "yield" ::: "memory" );
#elif defined( __ia64__ )
    asm volatile ( "hint @pause" ::: "memory" );
#elif defined( __GNUC__ )
    asm volatile( "" ::: "memory" );
#elif defined( _MSC_VER )
#   if defined( _M_ARM )
        YieldProcessor();
#   else
        _mm_pause();
#   endif
#endif
}

void nops( std::uint8_t const count ) noexcept { for ( auto i{ 0 }; i < count; ++i ) nop(); }

// http://locklessinc.com/articles/locks
void spin_lock::lock    () noexcept { while ( BOOST_UNLIKELY( !try_lock() ) ) [[ unlikely ]] { nop(); } }
bool spin_lock::try_lock() noexcept { return !flag_.test_and_set( std::memory_order_acquire ); }
void spin_lock::unlock  () noexcept { flag_.clear( std::memory_order_release ); }

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
