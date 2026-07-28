////////////////////////////////////////////////////////////////////////////////
///
/// \file semaphore.hpp
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
#include "futex.hpp"
#include "hardware_concurrency.hpp"

#include <atomic>
#include <cstdint>
#include <type_traits>

//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

class semaphore
{
public:
    semaphore() noexcept = default;
#ifndef NDEBUG
   ~semaphore() noexcept;
#endif // !NDEBUG

    void signal( hardware_concurrency_t count = 1 ) noexcept;

    void wait(                          ) noexcept;
    void wait( std::uint32_t spin_count ) noexcept;

// futex-backed on every platform (Linux & Windows natively; Apple through the
// __ulock futex backend in apple/futex.cpp).

private:
    using signed_futex_value_t =  std::make_signed_t< futex::value_type >;
    enum state : signed_futex_value_t { locked = 0, contested = -1 };

    signed_futex_value_t load( std::memory_order ) const noexcept;

    bool try_decrement( signed_futex_value_t & last_value ) noexcept;

private:
    futex                               value_   = { state::locked };
    std::atomic<hardware_concurrency_t> waiters_ = 0                ;


}; // class semaphore

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
