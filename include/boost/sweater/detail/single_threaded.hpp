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

#include <boost/core/no_exceptions_support.hpp>

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
    void spread_the_sweat( iterations_t const iterations, F && __restrict work ) noexcept( noexcept( std::declval< F >()( 0, 42 ) ) )
    {
        work( static_cast< iterations_t >( 0 ), iterations );
    }

    template <typename F>
    static void fire_and_forget( F && work )
    {
#   if defined( __EMSCRIPTEN__ ) && !defined( __EMSCRIPTEN_PTHREADS__ )
        work();
#   else
        std::thread( std::forward<F>( work ) ).detach();
#   endif
    }

    template <typename F>
    static auto dispatch( F && work )
    {
#   if defined( __EMSCRIPTEN__ ) && !defined( __EMSCRIPTEN_PTHREADS__ )
        using result_t = typename std::result_of<F()>::type;
        std::promise< result_t > promise;
        std::future < result_t > future( promise.get_future() );
        fire_and_forget
        (
            [promise = std::move( promise ), work = std::forward<F>( work )]
            () mutable noexcept
            {
                BOOST_TRY
                {
                    if constexpr ( std::is_same_v<result_t, void> )
                    {
                        work();
                        promise.set_value();
                    }
                    else
                    {
                        promise.set_value( work() );
                    }
                }
                BOOST_CATCH( ... )
                {
                    promise.set_exception( std::current_exception() );
                }
                BOOST_CATCH_END
            }
        );
        return future;
#   else
        return std::async( std::launch::async | std::launch::deferred, std::forward<F>( work ) );
#   endif
    }
}; // class shop

//------------------------------------------------------------------------------
} // namespace single_threaded
//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
