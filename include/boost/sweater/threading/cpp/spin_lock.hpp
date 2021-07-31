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
#include <cstdint>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

namespace detail
{
    template <typename T> void  overflow_checked_add( std::atomic<T> &, T value ) noexcept;
    template <typename T> void  overflow_checked_inc( std::atomic<T> &          ) noexcept;
    template <typename T> void underflow_checked_dec( std::atomic<T> &          ) noexcept;
} // namespace detail

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
