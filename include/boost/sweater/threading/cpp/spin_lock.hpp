////////////////////////////////////////////////////////////////////////////////
///
/// \file spin_lock.hpp
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
#pragma once
//------------------------------------------------------------------------------
#include <atomic>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

void nop() noexcept;
void nops( std::uint8_t count ) noexcept;

class spin_lock
{
public:
    spin_lock(                   ) noexcept = default;
    spin_lock( spin_lock const & ) = delete;

    void lock    () noexcept;
    bool try_lock() noexcept;
    void unlock  () noexcept;

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
}; // class spin_lock

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
