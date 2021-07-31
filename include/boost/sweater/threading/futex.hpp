////////////////////////////////////////////////////////////////////////////////
///
/// \file futex.hpp
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
#pragma once
//------------------------------------------------------------------------------
#include "hardware_concurrency.hpp"

#include <atomic>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

// https://www.remlab.net/op/futex-misc.shtml
// https://dept-info.labri.fr/~denis/Enseignement/2008-IR/Articles/01-futex.pdf

struct futex : std::atomic
<
#ifdef _WIN32
    hardware_concurrency_t
#else
    std::uint32_t
#endif
>
{
    void wake_one(                                        ) const noexcept;
    void wake    ( hardware_concurrency_t waiters_to_wake ) const noexcept;
    void wake_all(                                        ) const noexcept;

    void wait_if_equal( value_type desired_value ) const noexcept;
}; // struct futex

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
