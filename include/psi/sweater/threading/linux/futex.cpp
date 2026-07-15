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
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

namespace
{
    static_assert( sizeof( futex ) == 4 );
    static_assert( futex::all_bits == FUTEX_BITSET_MATCH_ANY );

    void futex( void const * const addr1, int const op, std::uint32_t const arg ) noexcept
    {
        ::syscall( SYS_futex, addr1, op | FUTEX_PRIVATE_FLAG, arg, nullptr, nullptr, 0 );
    }

    // val3 (bitset) variants: FUTEX_WAIT_BITSET/FUTEX_WAKE_BITSET behave exactly like
    // FUTEX_WAIT/FUTEX_WAKE when the bitset is FUTEX_BITSET_MATCH_ANY (== futex::all_bits),
    // so these subsume the plain ops above -- kept separate (rather than always going
    // through this path) only so a build with an older/stripped-down <linux/futex.h> that
    // happens to lack the _BITSET op codes can still fall back to the plain ops for
    // callers that never pass a real bitset (see futex_wait/futex_wake below, unchanged).
    void futex_bitset( void const * const addr1, int const op, std::uint32_t const arg, std::uint32_t const bitset ) noexcept
    {
        // FUTEX_WAIT_BITSET's timeout arg (here always null = block indefinitely) is
        // interpreted as an ABSOLUTE time if non-null (unlike FUTEX_WAIT's relative
        // timeout) -- irrelevant here since this codebase never passes one.
        ::syscall( SYS_futex, addr1, op | FUTEX_PRIVATE_FLAG, arg, nullptr, nullptr, bitset );
    }

    void futex_wait( void const * const addr, std::uint32_t const val             ) noexcept { return futex( addr, FUTEX_WAIT, val             ); }
	void futex_wake( void const * const addr, std::uint32_t const waiters_to_wake ) noexcept { return futex( addr, FUTEX_WAKE, waiters_to_wake ); }
} // anonymous namespace

void futex::wake_one(                                              ) const noexcept { wake( 1 ); }
void futex::wake    ( hardware_concurrency_t const waiters_to_wake ) const noexcept { futex_wake( this, waiters_to_wake ); }
void futex::wake_all( value_type const wake_bitset ) const noexcept
{
    if ( wake_bitset == all_bits ) { futex_wake( this, INT_MAX ); return; } // plain FUTEX_WAKE fast path
    futex_bitset( this, FUTEX_WAKE_BITSET, INT_MAX, wake_bitset );
}

void futex::wait_if_equal( value_type const desired_value, value_type const listen_bits ) const noexcept
{
    if ( listen_bits == all_bits ) { futex_wait( this, desired_value ); return; } // plain FUTEX_WAIT fast path
    futex_bitset( this, FUTEX_WAIT_BITSET, desired_value, listen_bits );
};

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
