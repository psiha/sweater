////////////////////////////////////////////////////////////////////////////////
///
/// \file spread_chunked.hpp
/// ------------------------
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
#include "threading/hardware_concurrency.hpp"

#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------

using iterations_t = std::uint32_t;

class chunked_spread
{
public:
    chunked_spread( iterations_t iterations, iterations_t number_of_chunks ) noexcept;

    std::pair<iterations_t, iterations_t> chunk_range( thrd_lite::hardware_concurrency_t chunk_index ) const noexcept;

private:
    iterations_t                      const iterations_per_chunk;
    thrd_lite::hardware_concurrency_t const extra_iterations;
#ifndef NDEBUG
    iterations_t                      const iterations;
#endif
}; // struct chunked_spread


/// For spreads that require limiting the number of work invocations to hardware_concurrency_max.
template <typename Shop, typename F>
void spread_chunked( Shop & shop, F && work, iterations_t const iterations ) noexcept
{
    auto const number_of_chunks{ thrd_lite::hardware_concurrency_max }; // to be turned into a parameter if needed later
    auto /*const*/ worker
    {
        [
            &work,
            setup = chunked_spread{ iterations, number_of_chunks }
        ]
        ( iterations_t const start_iteration, iterations_t const end_iteration ) noexcept
        {
            auto const chunk_index{ static_cast<thrd_lite::hardware_concurrency_t>( start_iteration ) };
            auto       range      { setup.chunk_range( chunk_index ) };
            //...mrmlj...hack to handle the generic impl falling back to performing everything on the caller thread
            if ( BOOST_UNLIKELY( end_iteration != start_iteration + 1 ) ) [[ unlikely ]]
            {
                auto const last_chunk_index{ static_cast<thrd_lite::hardware_concurrency_t>( end_iteration - 1 ) };
                auto const last_chunk_range{ setup.chunk_range( last_chunk_index ) };
                range.second = last_chunk_range.second;
            }
            work( range.first, range.second );
        }
    };
    /// \note The chunk_range logic does not fully cover the cases where the
    /// number of iterations is less than the number of workers (resulting
    /// in work being called with start_iteration > stop_iteration) so we
    /// have to additionally clamp the iterations parameter).
    ///                                       (12.01.2017.) (Domagoj Saric)
    shop.spread_the_sweat( std::min<iterations_t>( number_of_chunks, iterations ), worker );
}

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
