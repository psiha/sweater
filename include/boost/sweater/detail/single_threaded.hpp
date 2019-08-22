////////////////////////////////////////////////////////////////////////////////
///
/// \file single_threaded.hpp
/// -------------------------
///
/// (c) Copyright Domagoj Saric 2019.
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

#include "../hardware_concurrency.hpp"

#include <cstdint>
#include <future>
#include <thread>
#include <utility>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------
namespace single_threaded
{
//------------------------------------------------------------------------------

class shop
{
public:
    using iterations_t = std::uint32_t;

    constexpr shop() noexcept {}

    static constexpr hardware_concurrency_t number_of_workers() noexcept { return 1; }

    template <typename F>
    void spread_the_sweat( iterations_t const iterations, F && __restrict work ) noexcept( work( 0, iterations ) )
    {
        work( 0, iterations );
    }

    template <typename F>
    static void fire_and_forget( F && work )
    {
        std::thread( std::forward<F>( work ) ).detach();
    }

    template <typename F>
    static auto dispatch( F && work )
    {
        return std::async( std::launch::async | std::launch::deferred, std::forward<F>( work ) );
    }
}; // class shop

//------------------------------------------------------------------------------
} // namespace single_threaded
//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
