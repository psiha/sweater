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
#include "../hardware_concurrency.hpp"

#include <atomic>
#include <cstdint>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

// Here we only use global semaphore objects so there is no need for the
// race-condition workaround described in the links below.
// http://git.musl-libc.org/cgit/musl/commit/?id=88c4e720317845a8e01aee03f142ba82674cd23d
// https://github.com/preshing/cpp11-on-multicore/blob/master/common/sema.h
// https://stackoverflow.com/questions/36094115/c-low-level-semaphore-implementation
// https://comp.programming.threads.narkive.com/IRKGW6HP/too-much-overhead-from-semaphores
// TODO: futex barrier
// https://github.com/forhappy/barriers/blob/master/futex-barrier.c
// https://www.remlab.net/op/futex-misc.shtml
// https://dept-info.labri.fr/~denis/Enseignement/2008-IR/Articles/01-futex.pdf
class futex_semaphore
{
private:
    enum state { locked = 0, contested = -1 };

public:
    futex_semaphore() noexcept = default;
#ifndef NDEBUG
   ~futex_semaphore() noexcept;
#endif // !NDEBUG

    void signal( hardware_concurrency_t count = 1 ) noexcept;

    void wait(                          ) noexcept;
    void wait( std::uint32_t spin_count ) noexcept;

private:
    bool try_decrement( std::int32_t & last_value ) noexcept;

private:
    std::atomic<std::int32_t          > value_   = state::locked;
    std::atomic<hardware_concurrency_t> waiters_ = 0;
}; // class futex_semaphore

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
