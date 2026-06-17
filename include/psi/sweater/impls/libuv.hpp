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

#include <cstdint>
#include <exception>
#include <future>
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

    template <typename F>
    static bool spread_the_sweat( iterations_t const iterations, F && work, iterations_t /*parallelizable_count*/ = 1 ) noexcept
    {
        static_assert( noexcept( work( iterations_t{ 0 }, iterations ) ), "F must be noexcept" );
        if ( iterations == 0 )
        {
            return true;
        }
        work( iterations_t{ 0 }, iterations );
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
