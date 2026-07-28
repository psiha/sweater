////////////////////////////////////////////////////////////////////////////////
/// \file libuv.cpp
/// ---------------
/// Non-template ('F-independent') parts of the libuv psi::sweater backend.
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "libuv.hpp"

#include "../spread_chunked.hpp"
#include "../threading/cpp/spin_lock.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <latch>
#include <limits>
#include <mutex>
//------------------------------------------------------------------------------
namespace psi::sweater::libuv
{
//------------------------------------------------------------------------------

namespace
{
    // Chunk lifecycle: pending -> queued        (queuer, loop thread)
    //                  pending -> done_inline   (caller steal, or queuer on uv_queue_work failure)
    // All transitions happen under the owning spread_node's spin lock — the
    // queuer's queue-and-mark step and the caller's steal are short critical
    // sections, so no intermediate 'queueing' state (nor any bespoke waiting)
    // is needed. A queued chunk either runs on a pool worker or is
    // uv_cancel-reclaimed and run by the caller; its after-work callback
    // fires in both cases.
    enum chunk_state : std::uint8_t { chunk_pending, chunk_queued, chunk_done_inline };
} // anonymous namespace

struct shop::spread_node
{
    using iterations_t = shop::iterations_t;

    struct req_bundle
    {
        void const      * p_work;
        iterations_t      start;
        iterations_t      end;
        std::latch      * p_latch;
        spread_invoke_t   invoke;
        spread_node     * p_node;
        uv_work_t         req  {};
        std::uint8_t      state{ chunk_pending }; // guarded by the node's `lock`
    };

    spread_node                * next;
    iterations_t           const count;
    std::atomic<std::uint32_t>   refs;
    thrd_lite::spin_lock         lock; // serializes queuer vs steal-back per spread
    req_bundle                   bundles[ 1 ]; // trailing storage: `count` entries

    [[ nodiscard ]] static std::size_t allocation_size( iterations_t const count ) noexcept
    {
        return sizeof( spread_node ) + ( count - 1 ) * sizeof( req_bundle );
    }

    // All per-spread state lives in this single allocation, freed when the
    // last reference drops: caller + queuer + one per successfully queued
    // chunk (released by its after-work callback), so no participant can
    // ever observe freed state. Stack storage is not an option: after-work
    // callbacks fire on later loop iterations, possibly after the spread
    // call itself has returned.
    [[ nodiscard ]] static spread_node * create( iterations_t const count ) noexcept
    {
        auto * const storage{ static_cast<spread_node *>( ::operator new( allocation_size( count ), std::nothrow ) ) };
        if ( !storage ) [[ unlikely ]]
        {
            return nullptr;
        }
        // Refs: caller + queuer.
        auto * const node{ new ( storage ) spread_node{ nullptr, count, 2 } };
        for ( iterations_t i{ 1 }; i < count; ++i ) // [0] constructed by the placement new above
        {
            new ( &node->bundles[ i ] ) req_bundle{};
        }
        return node;
    }

    static void release( spread_node * const node ) noexcept
    {
        if ( node->refs.fetch_sub( 1, std::memory_order_acq_rel ) != 1 )
        {
            return;
        }
        // Trailing bundles beyond [0] are never explicitly destroyed.
        static_assert( std::is_trivially_destructible_v<req_bundle> );
        node->~spread_node();
        ::operator delete( node );
    }
}; // struct shop::spread_node

void shop::spread_work_cb( uv_work_t * const req ) noexcept
{
    auto const & bundle{ *static_cast<spread_node::req_bundle *>( req->data ) };
    bundle.invoke( bundle.p_work, bundle.start, bundle.end );
    bundle.p_latch->count_down();
}

void shop::spread_after_cb( uv_work_t * const req, int /*status: 0 or UV_ECANCELED — chunk ran either way (worker or steal-back)*/ ) noexcept
{
    spread_node::release( static_cast<spread_node::req_bundle *>( req->data )->p_node );
}


void shop::bind_loop( uv_loop_t * const loop ) noexcept
{
    BOOST_ASSERT_MSG( !loop_.load( std::memory_order_relaxed ), "bind_loop called twice" );
    BOOST_ASSERT_MSG( loop, "bind_loop called with a null loop" );
    loop_thread_ = uv_thread_self();
    async_.data  = this;
    if ( uv_async_init( loop, &async_, &on_spread_async ) != 0 ) [[ unlikely ]]
    {
        return; // leave loop_ null — spreads degrade to serial, fire* still asserts
    }
    // The bridge handle must not keep an otherwise-finished loop alive.
    uv_unref( reinterpret_cast<uv_handle_t *>( &async_ ) );
    loop_.store( loop, std::memory_order_release );
}

void shop::unbind_loop() noexcept
{
    if ( loop_.load( std::memory_order_relaxed ) ) // implies the async bridge handle is live
    {
#ifndef NDEBUG
        auto self_thread{ uv_thread_self() };
        BOOST_ASSERT_MSG( uv_thread_equal( &self_thread, &loop_thread_ ), "unbind_loop must run on the loop thread (uv_close is not thread-safe)" );
#endif
        uv_close( reinterpret_cast<uv_handle_t *>( &async_ ), nullptr );
        loop_.store( nullptr, std::memory_order_release );
    }
}

hardware_concurrency_t shop::number_of_workers() noexcept
{
    // libuv offers no API to query its thread-pool size — mirror the env
    // logic from uv/threadpool.c (default 4; libuv caps at 1024, further
    // clamped here to hardware_concurrency_t's range). Latched on first
    // call, matching libuv's own once-only pool initialization.
    static hardware_concurrency_t const pool_size{ []() noexcept -> hardware_concurrency_t
    {
        int size{ 4 };
        if ( auto const * const env{ std::getenv( "UV_THREADPOOL_SIZE" ) } )
        {
            if ( auto const parsed{ std::atoi( env ) }; parsed > 0 )
            {
                size = parsed;
            }
        }
        size = std::min( size, 1024 ); // libuv's own MAX_THREADPOOL_SIZE cap (uv/threadpool.c)
        return static_cast<hardware_concurrency_t>( std::min( size,
            static_cast<int>( std::numeric_limits<hardware_concurrency_t>::max() ) ) );
    }() };
    return pool_size;
}

void shop::spread_impl( iterations_t const iterations, void const * const p_work, spread_invoke_t const invoke ) noexcept
{
    if ( iterations == 0 ) [[ unlikely ]]
    {
        return;
    }

    if ( !loop_.load( std::memory_order_relaxed ) ) [[ unlikely ]]
    {
        invoke( p_work, iterations_t{ 0 }, iterations );
        return;
    }

    auto const num_workers{ number_of_workers() };
    // chunked_spread (and its chunk_range indices) are hardware_concurrency_t
    // sized — clamp the chunk count to the type's range as well (the 4x
    // oversubdivision would overflow it for pool sizes near the type max).
    auto const num_chunks { static_cast<iterations_t>( std::min({
        iterations,
        static_cast<iterations_t>( 4 * num_workers ),
        static_cast<iterations_t>( std::numeric_limits<hardware_concurrency_t>::max() )
    }) ) };

    if ( num_chunks <= 1 ) [[ unlikely ]]
    {
        invoke( p_work, iterations_t{ 0 }, iterations );
        return;
    }

    chunked_spread const setup        { iterations, num_chunks };
    auto           const queued_chunks{ static_cast<iterations_t>( num_chunks - 1 ) };

    // On allocation failure fall back to serial — never a plain queue-and-wait.
    auto * const node{ spread_node::create( queued_chunks ) };
    if ( !node ) [[ unlikely ]]
    {
        invoke( p_work, iterations_t{ 0 }, iterations );
        return;
    }

    std::latch sync{ static_cast<std::ptrdiff_t>( num_chunks ) };

    for ( iterations_t i{ 0 }; i < queued_chunks; ++i )
    {
        auto const [start, end]{ setup.chunk_range( static_cast<hardware_concurrency_t>( i ) ) };
        auto & bundle{ node->bundles[ i ] };
        bundle.p_work  = p_work;
        bundle.start   = start;
        bundle.end     = end;
        bundle.p_latch = &sync;
        bundle.invoke  = invoke;
        bundle.p_node  = node;
    }

    auto self_thread{ uv_thread_self() };
    if ( uv_thread_equal( &self_thread, &loop_thread_ ) )
    {
        queue_chunks( node );
    }
    else
    {
        auto * expected{ pending_.load( std::memory_order_relaxed ) };
        do
        {
            node->next = expected;
        } while ( !pending_.compare_exchange_weak( expected, node, std::memory_order_release, std::memory_order_relaxed ) );
        uv_async_send( &async_ );
    }

    {   // Caller participation: run the tail chunk inline.
        auto const [start, end]{ setup.chunk_range( static_cast<hardware_concurrency_t>( queued_chunks ) ) };
        invoke( p_work, start, end );
        sync.count_down();
    }

    // Steal-back pass, tail first (the queuer works head first). Each claim is
    // a short critical section under the node's spin lock (shared with the
    // queuer) — a pending chunk is claimed outright, a queued one is reclaimed
    // iff uv_cancel still can (its after-work callback then fires with
    // UV_ECANCELED and drops the queuer's ref); either way the chunk runs
    // outside the lock.
    for ( auto i{ queued_chunks }; i-- > 0; )
    {
        auto & bundle{ node->bundles[ i ] };
        bool run_inline{ false };
        {
            std::scoped_lock const guard{ node->lock };
            if ( bundle.state == chunk_pending )
            {
                bundle.state = chunk_done_inline;
                run_inline   = true;
            }
            else
            if ( ( bundle.state == chunk_queued ) && ( uv_cancel( reinterpret_cast<uv_req_t *>( &bundle.req ) ) == 0 ) )
            {
                bundle.state = chunk_done_inline;
                run_inline   = true;
            }
        }
        if ( run_inline )
        {
            bundle.invoke( bundle.p_work, bundle.start, bundle.end );
            sync.count_down();
        }
    }

    sync.wait();
    spread_node::release( node );
}

void shop::queue_chunks( spread_node * const node ) noexcept
{
    for ( iterations_t i{ 0 }; i < node->count; ++i )
    {
        auto & bundle{ node->bundles[ i ] };
        bool run_inline{ false };
        {
            std::scoped_lock const guard{ node->lock };
            if ( bundle.state != chunk_pending )
            {
                continue; // stolen by the caller
            }
            node->refs.fetch_add( 1, std::memory_order_relaxed ); // for the after-work callback
            bundle.req.data = &bundle;
            if ( uv_queue_work( loop_.load( std::memory_order_relaxed ), &bundle.req, &spread_work_cb, &spread_after_cb ) == 0 ) [[ likely ]]
            {
                bundle.state = chunk_queued;
            }
            else
            {   // cannot queue — run inline below (rare; executes on the loop thread)
                node->refs.fetch_sub( 1, std::memory_order_relaxed );
                bundle.state = chunk_done_inline;
                run_inline   = true;
            }
        }
        if ( run_inline ) [[ unlikely ]]
        {
            bundle.invoke( bundle.p_work, bundle.start, bundle.end );
            bundle.p_latch->count_down();
        }
    }
    spread_node::release( node );
}

void shop::on_spread_async( uv_async_t * const handle ) noexcept
{
    auto & self{ *static_cast<shop *>( handle->data ) };
    auto * node{ self.pending_.exchange( nullptr, std::memory_order_acquire ) };
    while ( node )
    {
        auto * const next{ node->next };
        self.queue_chunks( node );
        node = next;
    }
}

//------------------------------------------------------------------------------
} // namespace psi::sweater::libuv
//------------------------------------------------------------------------------
