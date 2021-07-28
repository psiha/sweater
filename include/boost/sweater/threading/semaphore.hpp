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
#include <boost/config.hpp>
#if defined( __linux__ )
#include "linux/semaphore.hpp"
#elif defined( BOOST_HAS_PTHREADS )
#include "posix/semaphore.hpp"
#else
#include "condvar.hpp"
#include "mutex.hpp"
#include "hardware_concurrency.hpp"

#include <atomic>
#include <mutex>
#endif
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

#if defined( __linux__ ) || defined( __EMSCRIPTEN__ )
using semaphore = futex_semaphore;
#elif defined( BOOST_HAS_PTHREADS )
using semaphore = pthread_semaphore;
#else
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

private:
    std::atomic<std::int32_t> value_      = 0; // atomic to support spin-waits
    hardware_concurrency_t    waiters_    = 0; // to enable detection when notify_all() can be used
    hardware_concurrency_t    to_release_ = 0;
    mutex                     mutex_    ;
    condition_variable        condition_;
}; // class semaphore
#endif

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
