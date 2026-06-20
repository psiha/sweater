////////////////////////////////////////////////////////////////////////////////
/// libuv thread-pool implementation of psi::sweater for Node.js embedders.
///
/// Uses `uv_queue_work` on the embedder's event loop so in-flight async work
/// is visible to libuv (no hand-rolled loop keep-alive). The after-work
/// callback runs on the loop thread — suitable for promise resolution without
/// a separate TaskRunner hop when the consumer wires `fire_with_after`.
////////////////////////////////////////////////////////////////////////////////
#pragma once
//------------------------------------------------------------------------------
#include "../detail/config.hpp"
#include "../dispatch_tracking.hpp"
#include "../spread_chunked.hpp"
#include "../threading/hardware_concurrency.hpp"
#include "../threading/thread.hpp"

#include <boost/assert.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <future>
#include <latch>
#include <type_traits>
#include <utility>

#include <uv.h>
//------------------------------------------------------------------------------
namespace psi::sweater::libuv
{
//------------------------------------------------------------------------------

using hardware_concurrency_t = thrd_lite::hardware_concurrency_t;

class shop
{
public:
    using iterations_t = std::uint32_t;

    shop() noexcept = default;

    void bind_loop( uv_loop_t * const loop ) noexcept { loop_ = loop; }

    [[ nodiscard ]] uv_loop_t * loop() const noexcept { return loop_; }

    [[ gnu::pure ]]
    static hardware_concurrency_t number_of_workers() noexcept
    {
        return thrd_lite::hardware_concurrency_max;
    }

    /// GCD dispatch_apply / Windows TP equivalent — synchronous parallel loop.
    /// Chunks via `chunked_spread`, queues each chunk on the libuv thread pool,
    /// and blocks until all chunks finish. `count_down` runs in the pool work
    /// callback so this is safe when called from a pool worker; if the bound
    /// loop is not set or only one chunk is needed, falls back to serial.
    template <typename F>
    bool spread_the_sweat( iterations_t const iterations, F && work, iterations_t /*parallelizable_count*/ = 1 ) noexcept
    {
        static_assert( noexcept( work( iterations_t{ 0 }, iterations ) ), "F must be noexcept" );

        if ( PSI_UNLIKELY( iterations == 0 ) )
        {
            return true;
        }

        if ( PSI_UNLIKELY( !loop_ ) )
        {
            work( iterations_t{ 0 }, iterations );
            return true;
        }

        auto const num_workers{ number_of_workers() };
        auto const num_chunks { static_cast<iterations_t>(
            std::min( iterations, static_cast<iterations_t>( 4 * num_workers ) )
        ) };

        if ( PSI_UNLIKELY( num_chunks <= 1 ) )
        {
            work( iterations_t{ 0 }, iterations );
            return true;
        }

        struct chunk_ctx
        {
            void const   * p_work;
            iterations_t   start;
            iterations_t   end;
            std::latch   * p_latch;
            void (*invoke)( void const *, iterations_t, iterations_t ) noexcept;
        };
        static_assert( std::is_trivially_destructible_v<chunk_ctx> );

        void (*const invoke_fn)( void const *, iterations_t, iterations_t ) noexcept =
            []( void const * pw, iterations_t s, iterations_t e ) noexcept
            {
                ( *static_cast<std::decay_t<F> const *>( pw ) )( s, e );
            };

        std::latch sync{ static_cast<std::ptrdiff_t>( num_chunks ) };

        chunked_spread const setup{ iterations, num_chunks };
        for ( iterations_t i{ 0 }; i < num_chunks; ++i )
        {
            auto const [start, end]{ setup.chunk_range( static_cast<hardware_concurrency_t>( i ) ) };

            struct req_bundle
            {
                chunk_ctx  ctx;
                uv_work_t  req{};
            };

            auto * const bundle{ new ( std::nothrow ) req_bundle{
                chunk_ctx{ &work, start, end, &sync, invoke_fn }
            } };
            if ( PSI_UNLIKELY( !bundle ) )
            {
                work( start, end );
                sync.count_down();
                continue;
            }

            bundle->req.data = bundle;
            if ( PSI_UNLIKELY( uv_queue_work(
                     loop_,
                     &bundle->req,
                     []( uv_work_t * const req ) noexcept
                     {
                         auto * const self{ static_cast<req_bundle *>( req->data ) };
                         auto & c{ self->ctx };
                         c.invoke( c.p_work, c.start, c.end );
                         c.p_latch->count_down();
                     },
                     []( uv_work_t * const req, int /*status*/ ) noexcept
                     {
                         delete static_cast<req_bundle *>( req->data );
                     }
                 ) != 0 ) )
            {
                work( start, end );
                sync.count_down();
                delete bundle;
            }
        }

        sync.wait();
        return true;
    }

    /// Fire `work` on the libuv thread pool (no loop-thread after callback).
    template <typename F>
    bool fire_and_forget( F && work ) noexcept( noexcept( std::is_nothrow_constructible_v<std::remove_reference_t<F>, F &&> ) )
    {
        return fire_with_after( std::forward<F>( work ), []() noexcept {} );
    }

    /// Run `work` on the libuv pool; `after` on the bound loop thread.
    template <typename Work, typename After>
    bool fire_with_after( Work && work, After && after ) noexcept
    (
        noexcept( std::is_nothrow_constructible_v<std::remove_reference_t<Work>, Work &&> ) &&
        noexcept( std::is_nothrow_constructible_v<std::remove_reference_t<After>, After &&> ) &&
        noexcept( std::declval<Work &>()() ) &&
        noexcept( std::declval<After &>()() )
    )
    {
        static_assert( noexcept( std::declval<Work &>()() ), "Work must be noexcept" );
        static_assert( noexcept( std::declval<After &>()() ), "After must be noexcept" );
        BOOST_ASSERT_MSG( loop_, "psi::sweater::libuv::shop::bind_loop() not called" );

        struct ctx
        {
            std::remove_reference_t<Work>  work;
            std::remove_reference_t<After> after;
            uv_work_t                      req{};
        };

        auto * const state{ new ( std::nothrow ) ctx{ std::forward<Work>( work ), std::forward<After>( after ) } };
        if ( !state )
        {
            return false;
        }
        state->req.data = state;
        detail::in_flight_inc();
        if ( uv_queue_work(
                 loop_,
                 &state->req,
                 []( uv_work_t * const req ) noexcept
                 {
                     static_cast<ctx *>( req->data )->work();
                 },
                 []( uv_work_t * const req, int /*status*/ ) noexcept
                 {
                     auto * const self{ static_cast<ctx *>( req->data ) };
                     self->after();
                     detail::in_flight_dec();
                     delete self;
                 }
             ) != 0 )
        {
            detail::in_flight_dec();
            delete state;
            return false;
        }
        return true;
    }

    using cpu_affinity_mask = thrd_lite::thread::affinity_mask;
    bool set_priority( thrd_lite::priority /*new_priority*/ ) noexcept { return true; }
    bool set_affinity( cpu_affinity_mask const & /*new_affinity*/ ) noexcept { return true; }

private:
    uv_loop_t * loop_{ nullptr };
}; // class shop

//------------------------------------------------------------------------------
} // namespace psi::sweater::libuv
//------------------------------------------------------------------------------
