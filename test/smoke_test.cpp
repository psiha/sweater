#include <psi/sweater/sweater.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <type_traits>

TEST( SweaterSmoke, SpreadTheSweat )
{
    std::atomic<int> sum{ 0 };
    psi::sweater::shop work_shop;
    work_shop.spread_the_sweat( 1000, [&]( auto const start, auto const end ) noexcept
    {
        for ( auto i{ start }; i < end; ++i )
        {
            sum.fetch_add( 1, std::memory_order_relaxed );
        }
    } );
    EXPECT_EQ( sum.load(), 1000 );
}

TEST( SweaterSmoke, DispatchReturnsValue )
{
    psi::sweater::shop work_shop;
    auto const future{ work_shop.dispatch( []() noexcept -> int { return 42; } ) };
    EXPECT_EQ( future.get(), 42 );
}

TEST( SweaterSmoke, FireAndForgetRunsWork )
{
    std::atomic<bool> done{ false };
    psi::sweater::shop work_shop;
    auto const run = [&]( auto && fn ) noexcept
    {
        if constexpr ( std::is_void_v<decltype( work_shop.fire_and_forget( fn ) )> )
            work_shop.fire_and_forget( std::forward<decltype( fn )>( fn ) );
        else
            (void)work_shop.fire_and_forget( std::forward<decltype( fn )>( fn ) );
    };
    run( [&]() noexcept { done.store( true, std::memory_order_release ); } );

    auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds( 5 ) };
    while ( !done.load( std::memory_order_acquire ) && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::yield();
    }
    EXPECT_TRUE( done.load() );
}
