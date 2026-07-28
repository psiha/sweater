////////////////////////////////////////////////////////////////////////////////
/// Cross-backend sweat_shop overhead microbenchmark.
///
/// Measures the pure queuing + dispatch cost of the shop implementations
/// available on the platform by pushing large numbers of no-op work items
/// through the two core entry points:
///   - fire:   N independent fire_and_forget no-ops (per-op enqueue/dispatch
///             cost, including the completion signal)
///   - spread: M spread_the_sweat calls over `iters` no-op iterations each
///             (per-spread setup/chunking/join cost — the work itself is free)
///
/// Backends: `generic` (sweater's own pool) always; the OS-provided pool where
/// one exists (Windows thread pool, Apple GCD); `libuv` when built against
/// libuv (SWEATER_BENCH_HAS_LIBUV, with a live loop thread like production
/// embedders have). Numbers are wall-clock — run on an idle machine and
/// compare ratios, not absolutes.
////////////////////////////////////////////////////////////////////////////////
#include <psi/sweater/impls/generic.hpp>
#ifdef _WIN32
#   include <psi/sweater/impls/windows.hpp>
#endif
#ifdef __APPLE__
#   include <psi/sweater/impls/apple.hpp>
#endif
#ifdef SWEATER_BENCH_HAS_LIBUV
#   include <psi/sweater/impls/libuv.hpp>
#   include <uv.h>
#endif

#include <psi/functionoid/function_ref.hpp>

#include <atomic>
#include <chrono>
#include <print>
#include <thread>
//------------------------------------------------------------------------------

namespace
{
    using clk = std::chrono::steady_clock;

    double ns( clk::duration const d, std::uint64_t const per ) noexcept
    {
        return std::chrono::duration_cast<std::chrono::duration<double, std::nano>>( d ).count() / static_cast<double>( per );
    }

    // `run_in_submit_context` executes the (timed) submission loop wherever the
    // backend's fire contract requires it — inline for the free-threaded pools,
    // trampolined onto the loop thread for libuv (whose fire_with_after must run
    // there; that is also what a real embedder's dispatch context looks like).
    template <typename Shop, typename RunInSubmitContext>
    void bench_fire( Shop & shop, char const * const name, std::uint32_t const n, RunInSubmitContext const & run_in_submit_context )
    {
        std::atomic<std::uint32_t> done{ 0 };
        auto const work{ [&done]() noexcept { done.fetch_add( 1, std::memory_order_relaxed ); } };

        auto const submit{ [&]
        {
            if constexpr ( std::is_void_v<decltype( shop.fire_and_forget( work ) )> )
            {
                shop.fire_and_forget( work );
            }
            else
            {   // bounded queues may refuse — retry (counts toward the overhead,
                // as it would in production)
                while ( !shop.fire_and_forget( work ) ) { std::this_thread::yield(); }
            }
        } };

        // Warmup (pool spin-up, queue allocation, page faults).
        run_in_submit_context( [&] { for ( std::uint32_t i{ 0 }; i < n / 10; ++i ) { submit(); } } );
        while ( done.load( std::memory_order_acquire ) != n / 10 ) { std::this_thread::yield(); }
        done.store( 0, std::memory_order_relaxed );

        clk::time_point t0, t_submitted;
        run_in_submit_context( [&]
        {
            t0 = clk::now();
            for ( std::uint32_t i{ 0 }; i < n; ++i ) { submit(); }
            t_submitted = clk::now();
        } );
        while ( done.load( std::memory_order_acquire ) != n ) { std::this_thread::yield(); }
        auto const t_done{ clk::now() };

        std::println( "  {:8} fire_and_forget x{} : {:8.1f} ns/op submit, {:8.1f} ns/op to completion",
            name, n, ns( t_submitted - t0, n ), ns( t_done - t0, n ) );
    }

    template <typename Shop>
    void bench_spread( Shop & shop, char const * const name, std::uint32_t const spreads, std::uint32_t const iters )
    {
        using iterations_t = typename Shop::iterations_t;
        auto const work{ []( iterations_t, iterations_t ) noexcept {} };

        for ( std::uint32_t m{ 0 }; m < spreads / 10; ++m ) { shop.spread_the_sweat( iters, work ); } // warmup

        auto const t0{ clk::now() };
        for ( std::uint32_t m{ 0 }; m < spreads; ++m ) { shop.spread_the_sweat( iters, work ); }
        auto const dt{ clk::now() - t0 };

        std::println( "  {:8} spread_the_sweat x{} ({} iters): {:8.1f} ns/spread",
            name, spreads, iters, ns( dt, spreads ) );
    }

    inline constexpr auto run_inline{ []( auto const & f ) { f(); } };

    template <typename Shop, typename RunInSubmitContext = decltype( run_inline ) const &>
    void bench_backend( Shop & shop, char const * const name, unsigned const workers, RunInSubmitContext const & run_in_submit_context = run_inline )
    {
        std::println( "{} ({} workers):", name, workers );
        bench_fire  ( shop, name, 200'000, run_in_submit_context );
        bench_spread( shop, name, 20'000, 1024 );
        bench_spread( shop, name, 200, 10'000'000 );
    }

#ifdef SWEATER_BENCH_HAS_LIBUV
    // Minimal live-loop runner mirroring what a real embedder provides: a
    // dedicated loop thread plus a dispatcher for marshalling closures onto it
    // (every uv handle is initialized and closed on the loop thread; only
    // uv_async_send crosses threads).
    struct loop_runner
    {
        uv_loop_t   loop{};
        uv_async_t  dispatcher{};
        std::thread thread;

        psi::functionoid::function_ref<void()> dispatched; // non-owning: run_on_loop blocks until the call completes
        std::atomic<bool>     dispatch_done{ false };
        std::atomic<bool>     stopped      { false };

        psi::sweater::libuv::shop shop;

        loop_runner()
        {
            uv_loop_init( &loop );
            std::atomic<bool> ready{ false };
            thread = std::thread{ [this, &ready]
            {
                dispatcher.data = this;
                uv_async_init( &loop, &dispatcher, +[]( uv_async_t * const h )
                {
                    auto & self{ *static_cast<loop_runner *>( h->data ) };
                    self.dispatched();
                    self.dispatch_done.store( true, std::memory_order_release );
                } );
                shop.bind_loop( &loop );
                ready.store( true, std::memory_order_release );
                uv_run( &loop, UV_RUN_DEFAULT ); // dispatcher handle keeps the loop alive
            } };
            while ( !ready.load( std::memory_order_acquire ) ) { std::this_thread::yield(); }
        }

        // Run `f` on the loop thread and wait for it. One caller at a time;
        // the referent only needs to outlive the (blocking) call.
        void run_on_loop( psi::functionoid::function_ref<void()> const f )
        {
            dispatched = f;
            dispatch_done.store( false, std::memory_order_relaxed );
            uv_async_send( &dispatcher );
            while ( !dispatch_done.load( std::memory_order_acquire ) ) { std::this_thread::yield(); }
        }

        ~loop_runner()
        {
            run_on_loop( [this]
            {
                shop.unbind_loop();
                uv_close( reinterpret_cast<uv_handle_t *>( &dispatcher ), +[]( uv_handle_t * const h )
                {
                    static_cast<loop_runner *>( h->data )->stopped.store( true, std::memory_order_release );
                } );
            } );
            while ( !stopped.load( std::memory_order_acquire ) ) { std::this_thread::yield(); }
            thread.join(); // uv_run returns once the last handle closes
            uv_loop_close( &loop );
        }
    }; // struct loop_runner
#endif // SWEATER_BENCH_HAS_LIBUV

} // anonymous namespace

int main()
{
    {
        psi::sweater::generic::shop shop;
        bench_backend( shop, "generic", shop.number_of_workers() );
    }
#ifdef _WIN32
    {
        psi::sweater::windows::shop shop;
        bench_backend( shop, "windows", shop.number_of_workers() );
    }
#endif
#ifdef __APPLE__
    {
        psi::sweater::apple::shop shop;
        bench_backend( shop, "apple", shop.number_of_workers() );
    }
#endif
#ifdef SWEATER_BENCH_HAS_LIBUV
    {
        loop_runner runner;
        bench_backend( runner.shop, "libuv", psi::sweater::libuv::shop::number_of_workers(),
            [&runner]( auto const & f ) { runner.run_on_loop( f ); } );
    }
#endif
    return 0;
}
//------------------------------------------------------------------------------
