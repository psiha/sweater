#include <psi/sweater/impls/libuv.hpp>
#include <psi/sweater/dispatch_tracking.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

namespace
{

struct loop_guard
{
    uv_loop_t loop{};
    bool      ok{ false };

    loop_guard() noexcept { ok = uv_loop_init( &loop ) == 0; }

    ~loop_guard() noexcept
    {
        if ( !ok )
        {
            return;
        }
        drain();
        uv_loop_close( &loop );
    }

    void drain() noexcept
    {
        while ( uv_loop_alive( &loop ) )
        {
            (void)uv_run( &loop, UV_RUN_ONCE );
        }
    }

    [[ nodiscard ]] uv_loop_t * get() noexcept { return ok ? &loop : nullptr; }
};

// Declare AFTER a loop_guard: destruction order then unbinds (closing the
// shop's async bridge handle) before the guard drains and closes the loop.
struct bound_shop
{
    psi::sweater::libuv::shop shop;

    explicit bound_shop( loop_guard & runner ) noexcept
    {
        if ( auto * const loop{ runner.get() } )
        {
            shop.bind_loop( loop );
        }
    }

    ~bound_shop() noexcept { shop.unbind_loop(); }

    psi::sweater::libuv::shop * operator->() noexcept { return &shop; }
};

} // namespace

TEST( SweaterLibuv, SpreadTheSweat_ParallelChunks )
{
    loop_guard runner;
    ASSERT_TRUE( runner.ok );

    bound_shop bound{ runner };
    auto & shop{ bound.shop };

    std::atomic<std::uint32_t> sum{ 0 };
    EXPECT_TRUE( shop.spread_the_sweat(
        1000,
        [&]( auto const start, auto const end ) noexcept
        {
            for ( auto i{ start }; i < end; ++i )
            {
                sum.fetch_add( 1, std::memory_order_relaxed );
            }
        }
    ) );
    runner.drain();
    EXPECT_EQ( sum.load(), 1000u );
}

TEST( SweaterLibuv, SpreadTheSweat_NoLoop_FallsBackSerial )
{
    psi::sweater::libuv::shop shop;

    std::atomic<std::uint32_t> sum{ 0 };
    EXPECT_TRUE( shop.spread_the_sweat(
        256,
        [&]( auto const start, auto const end ) noexcept
        {
            for ( auto i{ start }; i < end; ++i )
            {
                sum.fetch_add( 1, std::memory_order_relaxed );
            }
        }
    ) );
    EXPECT_EQ( sum.load(), 256u );
}

TEST( SweaterLibuv, FireWithAfter_RunsWorkAndAfter )
{
    loop_guard runner;
    ASSERT_TRUE( runner.ok );

    bound_shop bound{ runner };
    auto & shop{ bound.shop };

    std::atomic<bool> work_done{ false };
    std::atomic<bool> after_done{ false };

    EXPECT_TRUE( shop.fire_with_after(
        [&]() noexcept { work_done.store( true, std::memory_order_release ); },
        [&]() noexcept { after_done.store( true, std::memory_order_release ); }
    ) );

    auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 5 } };
    while ( !after_done.load( std::memory_order_acquire ) && std::chrono::steady_clock::now() < deadline )
    {
        (void)uv_run( runner.get(), UV_RUN_ONCE );
    }

    EXPECT_TRUE( work_done.load( std::memory_order_acquire ) );
    EXPECT_TRUE( after_done.load( std::memory_order_acquire ) );
    EXPECT_EQ( psi::sweater::detail::in_flight_count(), 0u );
}

TEST( SweaterLibuv, FireAndForget_RunsWork )
{
    loop_guard runner;
    ASSERT_TRUE( runner.ok );

    bound_shop bound{ runner };
    auto & shop{ bound.shop };

    std::atomic<bool> done{ false };
    EXPECT_TRUE( shop.fire_and_forget( [&]() noexcept { done.store( true, std::memory_order_release ); } ) );

    auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 5 } };
    while ( !done.load( std::memory_order_acquire ) && std::chrono::steady_clock::now() < deadline )
    {
        (void)uv_run( runner.get(), UV_RUN_ONCE );
    }

    EXPECT_TRUE( done.load( std::memory_order_acquire ) );
    runner.drain();
    EXPECT_EQ( psi::sweater::detail::in_flight_count(), 0u );
}

// Regression: a spread issued FROM a pool worker must not deadlock even when
// every pool slot is occupied by such a spreading task. Saturate the pool with
// number_of_workers() fire_and_forget tasks that each spread — with the old
// queue-and-wait implementation every worker blocked on chunks that had no
// free slot to run on; the steal-back pass makes each caller reclaim and run
// its own chunks inline.
TEST( SweaterLibuv, SpreadTheSweat_FromSaturatedPoolWorkers_NoDeadlock )
{
    loop_guard runner;
    ASSERT_TRUE( runner.ok );

    bound_shop bound{ runner };
    auto & shop{ bound.shop };

    auto const spreaders{ static_cast<std::uint32_t>( shop.number_of_workers() ) };
    constexpr std::uint32_t iterations_per_spread{ 1000 };

    std::atomic<std::uint32_t> sum      { 0 };
    std::atomic<std::uint32_t> completed{ 0 };

    for ( std::uint32_t s{ 0 }; s < spreaders; ++s )
    {
        ASSERT_TRUE( shop.fire_and_forget(
            [&]() noexcept
            {
                (void)shop.spread_the_sweat(
                    iterations_per_spread,
                    [&]( auto const start, auto const end ) noexcept
                    {
                        for ( auto i{ start }; i < end; ++i )
                        {
                            sum.fetch_add( 1, std::memory_order_relaxed );
                        }
                    }
                );
                completed.fetch_add( 1, std::memory_order_acq_rel );
            }
        ) );
    }

    auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 30 } };
    while ( completed.load( std::memory_order_acquire ) != spreaders && std::chrono::steady_clock::now() < deadline )
    {
        (void)uv_run( runner.get(), UV_RUN_ONCE );
    }

    ASSERT_EQ( completed.load( std::memory_order_acquire ), spreaders ) << "deadlocked: spreads from saturated pool workers never finished";
    EXPECT_EQ( sum.load(), spreaders * iterations_per_spread );
    runner.drain();
    EXPECT_EQ( psi::sweater::detail::in_flight_count(), 0u );
}

TEST( SweaterLibuv, NumberOfWorkers_MatchesLibuvPoolSizing )
{
    auto const reported{ psi::sweater::libuv::shop::number_of_workers() };
    // Mirrors uv/threadpool.c: default 4 unless UV_THREADPOOL_SIZE overrides.
    if ( auto const * const env{ std::getenv( "UV_THREADPOOL_SIZE" ) }; env && std::atoi( env ) > 0 )
    {
        // Same clamp as the implementation: hardware_concurrency_t's range
        // (no separate hardcoded cap — keeps the test in lockstep should the
        // type ever widen).
        auto const expected{ std::min( std::atoi( env ),
            static_cast<int>( std::numeric_limits<psi::sweater::libuv::hardware_concurrency_t>::max() ) ) };
        EXPECT_EQ( static_cast<int>( reported ), expected );
    }
    else
    {
        EXPECT_EQ( reported, 4 );
    }
}
