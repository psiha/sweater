#include <psi/sweater/impls/libuv.hpp>
#include <psi/sweater/dispatch_tracking.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
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

} // namespace

TEST( SweaterLibuv, SpreadTheSweat_ParallelChunks )
{
    loop_guard runner;
    ASSERT_TRUE( runner.ok );

    psi::sweater::libuv::shop shop;
    shop.bind_loop( runner.get() );

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

    psi::sweater::libuv::shop shop;
    shop.bind_loop( runner.get() );

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

    psi::sweater::libuv::shop shop;
    shop.bind_loop( runner.get() );

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
