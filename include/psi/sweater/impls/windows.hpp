////////////////////////////////////////////////////////////////////////////////
///
/// \file windows.hpp
/// -----------------
///
/// Windows Thread Pool API implementation of psi::sweater.
/// Uses the default system thread pool (NULL TP_CALLBACK_ENVIRON) so no
/// per-instance state is needed — shop is stateless, like the apple.hpp impl.
///
/// (c) Copyright Domagoj Saric 2016 - 2025.
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
#include "../detail/config.hpp"
#include "../spread_chunked.hpp"
#include "../threading/hardware_concurrency.hpp"

#include <boost/assert.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <future>
#include <latch>
#include <memory>
#include <type_traits>

#ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <threadpoolapiset.h>
//------------------------------------------------------------------------------
namespace psi::sweater::windows
{
//------------------------------------------------------------------------------

using hardware_concurrency_t = thrd_lite::hardware_concurrency_t;

class shop
{
public:
    using iterations_t = std::uint32_t;

    [[ gnu::pure ]]
    static hardware_concurrency_t number_of_workers() noexcept
    {
        return thrd_lite::hardware_concurrency_max;
    }

    /// GCD dispatch_apply equivalent — synchronous parallel loop.
    /// Splits into 4×number_of_workers chunks for work-stealing headroom.
    template <typename F>
    static bool spread_the_sweat( iterations_t const iterations, F && __restrict work, iterations_t /*parallelizable_count*/ = 1 ) noexcept
    {
        static_assert( noexcept( work( iterations, iterations ) ), "F must be noexcept" );

        if ( PSI_UNLIKELY( iterations == 0 ) )
            return true;

        auto const num_workers{ number_of_workers() };
        auto const num_chunks { static_cast<iterations_t>(
            std::min( iterations, static_cast<iterations_t>( 4 * num_workers ) )
        ) };

        // Type-erased chunk descriptor — trivially destructible so _alloca is safe.
        struct chunk_ctx
        {
            void const   * p_work;
            iterations_t   start;
            iterations_t   end;
            std::latch   * p_latch;
            void (*invoke)( void const *, iterations_t, iterations_t ) noexcept;
        };
        static_assert( std::is_trivially_destructible_v<chunk_ctx> );

        // Type-erase F into a plain function pointer once, reuse for all chunks.
        void (*const invoke_fn)( void const *, iterations_t, iterations_t ) noexcept =
            []( void const * pw, iterations_t s, iterations_t e ) noexcept
            {
                ( *static_cast<std::decay_t<F> const *>( pw ) )( s, e );
            };

        std::latch sync{ static_cast<std::ptrdiff_t>( num_chunks ) };

        // Stack-allocate chunk contexts — safe because sync.wait() keeps this frame alive.
        auto * const ctxs{ static_cast<chunk_ctx *>( _alloca( num_chunks * sizeof( chunk_ctx ) ) ) };

        chunked_spread const setup{ iterations, num_chunks };
        for ( iterations_t i{ 0 }; i < num_chunks; ++i )
        {
            auto const [start, end]{ setup.chunk_range( static_cast<hardware_concurrency_t>( i ) ) };
            ctxs[ i ] = chunk_ctx{ &work, start, end, &sync, invoke_fn };
            if ( PSI_UNLIKELY( !TrySubmitThreadpoolCallback(
                []( PTP_CALLBACK_INSTANCE, PVOID ctx ) noexcept
                {
                    auto const & c{ *static_cast<chunk_ctx const *>( ctx ) };
                    c.invoke( c.p_work, c.start, c.end );
                    c.p_latch->count_down();
                },
                &ctxs[ i ],
                nullptr // default system thread pool
            ) ) )
            {
                // Pool exhausted — run inline on the caller rather than hang.
                work( start, end );
                sync.count_down();
            }
        }

        sync.wait();
        return true;
    }

    template <typename F>
    static bool fire_and_forget( F && work ) noexcept
    {
        static_assert( noexcept( std::declval<std::decay_t<F> &>()() ), "fire_and_forget work must be noexcept" );
        using Functor = std::remove_reference_t<F>;

        if constexpr (
            ( sizeof ( Functor ) <= sizeof ( void * ) ) &&
            ( alignof( Functor ) <= alignof( void * ) ) &&
            std::is_trivially_copyable_v    <Functor> &&
            std::is_trivially_destructible_v<Functor>
        )
        {
            // Small trivially-copyable functor — pack directly into the context pointer.
            void * ctx{ nullptr };
            new ( &ctx ) Functor( std::forward<F>( work ) );
            auto const submitted{ TrySubmitThreadpoolCallback(
                []( PTP_CALLBACK_INSTANCE, PVOID ctx ) noexcept
                {
                    reinterpret_cast<Functor &>( ctx )();
                },
                ctx,
                nullptr
            ) != FALSE };
            if ( PSI_UNLIKELY( !submitted ) )
            {
                reinterpret_cast<Functor &>( ctx )(); // run inline on pool exhaustion
            }
            return submitted;
        }
        else
        {
            // Non-trivial or large functor — heap-allocate, callback deletes it.
            auto * const p{ new ( std::nothrow ) Functor( std::forward<F>( work ) ) };
            if ( PSI_UNLIKELY( !p ) )
                return false;

            auto const submitted{ TrySubmitThreadpoolCallback(
                []( PTP_CALLBACK_INSTANCE, PVOID ctx ) noexcept
                {
                    auto * const pf{ static_cast<Functor *>( ctx ) };
                    ( *pf )();
                    delete pf;
                },
                p,
                nullptr
            ) != FALSE };
            if ( PSI_UNLIKELY( !submitted ) )
            {
                ( *p )(); // run inline on pool exhaustion
                delete p;
            }
            return submitted;
        }
    }

    template <typename F>
    static auto dispatch( F && work )
    {
        using result_t  = std::invoke_result_t<std::decay_t<F> &>;
        using promise_t = std::promise<result_t>;
        using future_t  = std::future <result_t>;

        promise_t promise;
        future_t  future{ promise.get_future() };

        fire_and_forget(
            [ p = std::move( promise ), f = std::forward<F>( work ) ]() mutable noexcept
            {
                try
                {
                    if constexpr ( std::is_same_v<result_t, void> )
                    {
                        f();
                        p.set_value();
                    }
                    else
                    {
                        p.set_value( f() );
                    }
                }
                catch ( ... )
                {
                    p.set_exception( std::current_exception() );
                }
            }
        );
        return future;
    }

}; // class shop

//------------------------------------------------------------------------------
} // namespace psi::sweater::windows
//------------------------------------------------------------------------------
