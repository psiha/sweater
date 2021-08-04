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

#ifdef __APPLE__ // not futex for you!
#include "condvar.hpp"
#include "mutex.hpp"

#include <mutex>
#endif // Apple
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
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

#if !defined( __APPLE__ ) /////////////////////////////////////////////////////

private:
    using signed_futex_value_t =  std::make_signed_t< futex::value_type >;
    enum state : signed_futex_value_t { locked = 0, contested = -1 };

    signed_futex_value_t load( std::memory_order ) const noexcept;

    bool try_decrement( signed_futex_value_t & last_value ) noexcept;

private:
    futex                               value_   = { state::locked };
    std::atomic<hardware_concurrency_t> waiters_ = 0                ;

#else // generic impl for futexless platforms  ////////////////////////////////

private:
    std::atomic<std::int32_t> value_      = 0; // atomic to support spin-waits
    hardware_concurrency_t    waiters_    = 0; // to enable detection when notify_all() can be used
    hardware_concurrency_t    to_release_ = 0;
    mutex                     mutex_    ;
    condition_variable        condition_;

#endif // Apple ///////////////////////////////////////////////////////////////
}; // class semaphore

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
