////////////////////////////////////////////////////////////////////////////////
///
/// \file condvar.hpp
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
#include <algorithm>
#include <chrono>
#include <cstdint>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// The range every platform's timed wait accepts, the Windows one:
// [0, INFINITE - 1] milliseconds (negative is zero, overlong is clamped).
[[ nodiscard ]] constexpr std::uint32_t clamp_timeout_ms( std::chrono::milliseconds const timeout ) noexcept
{
    return static_cast<std::uint32_t>( std::clamp<std::chrono::milliseconds::rep>( timeout.count(), 0, 0xFFFF'FFFEu ) );
}

// The predicate-loop wait shared by all platform condition variables (CRTP:
// the derived class provides wait( lock, std::chrono::milliseconds ) returning
// false on timeout).
template <typename Derived>
class condvar_wait_until
{
public:
    // Waits until pred() holds or the deadline passes; returns pred()'s final value.
    template <typename Lock, typename Pred>
    bool wait_until( Lock & lock, std::chrono::steady_clock::time_point const deadline, Pred pred ) noexcept( noexcept( pred() ) )
    {
        while ( !pred() )
        {
            auto const remaining{ deadline - std::chrono::steady_clock::now() };
            if ( remaining <= remaining.zero() )
                return false;
            static_cast<Derived &>( *this ).wait( lock, std::chrono::ceil<std::chrono::milliseconds>( remaining ) );
        }
        return true;
    }
};

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------

// (after the helpers above: the platform classes build on them)
#if !defined( _WIN32 )
#include "posix/condvar.hpp"
#else
#include "windows/condvar.hpp"
#endif
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

#if !defined( _WIN32 )
using condition_variable = pthread_condition_variable;
#else
using condition_variable = win32_condition_variable;
#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
