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
#include "../threading/hardware_concurrency.hpp"
#include "../threading/thread.hpp"

#include <boost/assert.hpp>

#include <atomic>
#include <cstdint>
#include <new>
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

    /// Must be called on the loop's own thread (it installs the async handle
    /// that off-loop spread_the_sweat callers use to reach the loop).
    void bind_loop( uv_loop_t * loop ) noexcept;

    /// Must be called on the loop thread, before the loop is closed (libuv
    /// asserts on open handles in uv_loop_close). Safe to call when never/
    /// no-longer bound. The close completes on a subsequent loop iteration.
    /// The host must quiesce dispatchers first: no spread_the_sweat /
    /// fire_with_after call may be in flight or start concurrently with (or
    /// after) this call — drain the pool (e.g. wait_until_idle()) before
    /// unbinding.
    void unbind_loop() noexcept;

    [[ nodiscard ]] uv_loop_t * loop() const noexcept { return loop_.load( std::memory_order_relaxed ); }

    [[ gnu::pure ]]
    static hardware_concurrency_t number_of_workers() noexcept;

    /// GCD dispatch_apply / Windows TP equivalent — synchronous parallel loop.
    ///
    /// Chunks via `chunked_spread`. Only `uv_async_send` is thread-safe in
    /// libuv, so chunks are queued on the loop thread: directly when the
    /// caller IS the loop thread, otherwise through an MPSC request list +
    /// the async bridge handle installed by `bind_loop`.
    ///
    /// The caller participates: it runs the tail chunk inline, then steals
    /// back and runs inline every chunk the pool has not picked up (claims
    /// are serialized against the queuer by a per-spread spin lock: not yet
    /// queued chunks are claimed outright, already queued ones via
    /// `uv_cancel`, which succeeds only for work still sitting in the pool
    /// queue — so each chunk executes exactly once, here or on a worker).
    /// This guarantees forward progress even when called from a pool worker
    /// with the pool fully saturated — the call can never deadlock waiting
    /// on slots it occupies; it degrades toward serial execution exactly as
    /// far as the pool is busy.
    ///
    /// If no loop is bound or only one chunk is needed, runs serially.
    template <typename F>
    bool spread_the_sweat( iterations_t const iterations, F && work, iterations_t /*parallelizable_count*/ = 1 ) noexcept
    {
        static_assert( noexcept( work( iterations_t{ 0 }, iterations ) ), "F must be noexcept" );
        spread_impl
        (
            iterations,
            std::addressof( work ),
            []( void const * const p_work, iterations_t const start, iterations_t const end ) noexcept
            {
                ( *static_cast<std::decay_t<F> const *>( p_work ) )( start, end );
            }
        );
        return true;
    }

    /// Fire `work` on the libuv thread pool (no loop-thread after callback).
    template <typename F>
    bool fire_and_forget( F && work ) noexcept( noexcept( std::is_nothrow_constructible_v<std::remove_reference_t<F>, F &&> ) )
    {
        return fire_with_after( std::forward<F>( work ), []() noexcept {} );
    }

    /// Run `work` on the libuv pool; `after` on the bound loop thread.
    /// Must be called ON the loop thread (`uv_queue_work` is not thread-safe;
    /// off-loop dispatch is what spread_the_sweat's async bridge is for).
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
        auto * const loop{ loop_.load( std::memory_order_relaxed ) };
        BOOST_ASSERT_MSG( loop, "psi::sweater::libuv::shop::bind_loop() not called" );
#ifndef NDEBUG
        {
            auto self_thread{ uv_thread_self() };
            BOOST_ASSERT_MSG( uv_thread_equal( &self_thread, &loop_thread_ ), "fire_with_after must be called on the bound loop thread (uv_queue_work is not thread-safe)" );
        }
#endif

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
                 loop,
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
    struct spread_node; // single-allocation per-spread state, defined in libuv.cpp

    using spread_invoke_t = void (*)( void const * p_work, iterations_t start, iterations_t end ) /*noexcept*/;

    /// The F-independent bulk of spread_the_sweat (defined in libuv.cpp).
    void spread_impl( iterations_t iterations, void const * p_work, spread_invoke_t invoke ) noexcept;

    /// Loop thread only.
    void queue_chunks( spread_node * node ) noexcept;

    static void spread_work_cb ( uv_work_t  * req ) noexcept;
    static void spread_after_cb( uv_work_t  * req, int status ) noexcept;
    static void on_spread_async( uv_async_t * handle ) noexcept;

private:
    // `loop_` is written on the loop thread (bind_loop / unbind_loop) and read
    // from arbitrary threads (spread_the_sweat / fire_with_after) — atomic to
    // keep those reads well-defined. Atomicity alone does NOT make teardown
    // safe against still-running calls (a caller could load the pointer just
    // before unbind_loop closes the bridge): the host must quiesce dispatchers
    // before unbinding — see unbind_loop().
    std::atomic<uv_loop_t *>   loop_        { nullptr };
    uv_thread_t                loop_thread_ {};
    uv_async_t                 async_       {};
    std::atomic<spread_node *> pending_     { nullptr }; // MPSC list of off-loop spread requests
}; // class shop

//------------------------------------------------------------------------------
} // namespace psi::sweater::libuv
//------------------------------------------------------------------------------
