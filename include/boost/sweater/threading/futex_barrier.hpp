////////////////////////////////////////////////////////////////////////////////
///
/// \file futex_barrier.hpp
/// -----------------------
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
#include "../impls/generic_config.hpp" //...mrmlj...spaghetti...

#include "futex.hpp"
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

class futex_barrier
{
public:
    futex_barrier() noexcept;
    futex_barrier( hardware_concurrency_t initial_value ) noexcept;
#ifndef NDEBUG
   ~futex_barrier() noexcept;
#endif // NDEBUG

    void initialize( hardware_concurrency_t initial_value ) noexcept;

    void add_expected_arrival() noexcept;

    hardware_concurrency_t actives         () const noexcept;
    bool                   everyone_arrived() const noexcept;

#if BOOST_SWEATER_USE_CALLER_THREAD
    void use_spin_wait( bool value ) noexcept;
#endif // BOOST_SWEATER_USE_CALLER_THREAD

    void arrive() noexcept;
    void wait  () noexcept;

#if BOOST_SWEATER_USE_CALLER_THREAD
    bool spin_wait( std::uint32_t nop_spin_count = 0 ) noexcept;
#endif // BOOST_SWEATER_USE_CALLER_THREAD

private:
    futex counter_;
#if BOOST_SWEATER_USE_CALLER_THREAD //...mrmlj...spaghetti...
    bool  spin_wait_{ false };
#endif // BOOST_SWEATER_USE_CALLER_THREAD
}; // class futex_barrier

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
