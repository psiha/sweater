////////////////////////////////////////////////////////////////////////////////
///
/// \file condvar.cpp
/// -----------------
///
/// (c) Copyright Domagoj Saric 2016 - 2026.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "../condvar.hpp"
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

bool win32_condition_variable::wait( mutex_t & m /*must be locked*/, std::chrono::milliseconds const timeout ) noexcept
{
    return wait( m, clamp_timeout_ms( timeout ) );
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
