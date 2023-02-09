////////////////////////////////////////////////////////////////////////////////
///
/// \file spread_chunked.cpp
/// ------------------------
///
/// (c) Copyright Domagoj Saric 2021 - 2023.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "spread_chunked.hpp"

#include <boost/assert.hpp>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------

using hardware_concurrency_t = thrd_lite::hardware_concurrency_t;

chunked_spread::chunked_spread( iterations_t const iterations, iterations_t const number_of_chunks ) noexcept
    :
    iterations_per_chunk{                                      iterations / number_of_chunks   },
    extra_iterations    { static_cast<hardware_concurrency_t>( iterations % number_of_chunks ) }
#ifndef NDEBUG
   ,iterations          { iterations }
#endif
{}

BOOST_NOINLINE
std::pair<iterations_t, iterations_t> chunked_spread::chunk_range( hardware_concurrency_t const chunk_index ) const noexcept
{
    auto         const extra_iters        { std::min( chunk_index, extra_iterations ) };
    iterations_t const plain_iters        ( chunk_index - extra_iters                 );
    auto         const this_has_extra_iter{ chunk_index < extra_iterations            };
    auto         const start_iteration
    {
        extra_iters * ( iterations_per_chunk + 1 )
            +
        plain_iters *   iterations_per_chunk
    };
    auto const stop_iteration{ start_iteration + iterations_per_chunk + this_has_extra_iter };
#ifndef NDEBUG
    BOOST_ASSERT( stop_iteration <= iterations );
#endif
    BOOST_ASSERT_MSG( start_iteration < stop_iteration, "Sweater internal inconsistency: worker called with no work to do." );
    return std::make_pair( start_iteration, stop_iteration );
}

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
