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
#ifndef __clang__
#pragma once
#endif
//------------------------------------------------------------------------------
#include "../futex.hpp"

#include <boost/assert.hpp>

#include <windows.h>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// TODO cpp20 version
void futex::wake_one() const noexcept { ::WakeByAddressSingle( const_cast< futex * >( this ) ); }
// wake_bitset ignored: WakeByAddress* has no per-waiter-category filtering concept --
// see futex.hpp's design-doc comment on the bitset parameter.
void futex::wake_all( value_type ) const noexcept { ::WakeByAddressAll( const_cast< futex * >( this ) ); }

void futex::wake( hardware_concurrency_t ) const noexcept { wake_all(); } // no exact-count API

// listen_bits ignored: WaitOnAddress has no bitset concept -- see futex.hpp.
void futex::wait_if_equal( value_type const desired_value, value_type ) const noexcept
{
    BOOST_VERIFY( ::WaitOnAddress( const_cast< futex * >( this ), const_cast< value_type * >( &desired_value ), sizeof( *this ), INFINITE ) );
};

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
