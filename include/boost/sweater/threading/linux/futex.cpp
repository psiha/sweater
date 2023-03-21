////////////////////////////////////////////////////////////////////////////////
///
/// \file futex.cpp
/// ---------------
///
/// (c) Copyright Domagoj Saric 2021.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "../futex.hpp"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <climits>

//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

namespace
{
    static_assert( sizeof( futex ) == 4 );

    void futex( void const * const addr1, int const op, std::uint32_t const arg ) noexcept
    {
        ::syscall( SYS_futex, addr1, op | FUTEX_PRIVATE_FLAG, arg, nullptr, nullptr, 0 );
    }

    void futex_wait( void const * const addr, std::uint32_t const val             ) noexcept { return futex( addr, FUTEX_WAIT, val             ); }
	void futex_wake( void const * const addr, std::uint32_t const waiters_to_wake ) noexcept { return futex( addr, FUTEX_WAKE, waiters_to_wake ); }
} // anonymous namespace

void futex::wake_one(                                              ) const noexcept { wake( 1 ); }
void futex::wake    ( hardware_concurrency_t const waiters_to_wake ) const noexcept { futex_wake( this, waiters_to_wake ); }
void futex::wake_all(                                              ) const noexcept { futex_wake( this, INT_MAX         ); }

void futex::wait_if_equal( value_type const desired_value ) const noexcept
{
    futex_wait( this, desired_value );
};

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
