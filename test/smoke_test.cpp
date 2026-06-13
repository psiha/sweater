#include <psi/sweater/sweater.hpp>

#include <gtest/gtest.h>

#include <atomic>

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
