//==============================================================================
// Tests for psi::thrd_lite::outcome_promise/outcome_future (outcome_future.hpp)
// -- the Outcome-based peer to thrd_lite::promise/future (future_test.cpp),
// independent of any sweater shop/dispatch_outcome() usage. Only built when
// PSI_SWEATER_WITH_OUTCOME=ON (see test/CMakeLists.txt).
//==============================================================================

#include <psi/sweater/threading/outcome_future.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

TEST( ThrdLiteOutcomeFuture, ValueSameThread )
{
    auto pair( make_outcome_promise_future<int>() );
    pair.first.set( 42 );
    auto const result( pair.second.get() );
    ASSERT_TRUE( result.has_value() );
    EXPECT_EQ( result.value(), 42 );
}

TEST( ThrdLiteOutcomeFuture, VoidValueSameThread )
{
    auto pair( make_outcome_promise_future<void>() );
    pair.first.set( detail::outcome_ns::success() );
    auto const result( pair.second.get() );
    EXPECT_TRUE( result.has_value() );
}

TEST( ThrdLiteOutcomeFuture, ExceptionSameThread )
{
    auto pair( make_outcome_promise_future<int>() );
    pair.first.set( outcome_result<int>{ std::make_exception_ptr( std::runtime_error{ "boom" } ) } );
    auto const result( pair.second.get() );
    ASSERT_TRUE( result.has_exception() );
    EXPECT_THROW( std::rethrow_exception( result.exception() ), std::runtime_error );
}

TEST( ThrdLiteOutcomeFuture, RunCapturesResult )
{
    auto pair( make_outcome_promise_future<int>() );
    pair.first.run( []() noexcept { return 7; } );
    auto const result( pair.second.get() );
    ASSERT_TRUE( result.has_value() );
    EXPECT_EQ( result.value(), 7 );
}

TEST( ThrdLiteOutcomeFuture, RunCapturesException )
{
    auto pair( make_outcome_promise_future<int>() );
    pair.first.run( []() -> int { throw std::runtime_error{ "boom" }; } );
    auto const result( pair.second.get() );
    ASSERT_TRUE( result.has_exception() );
    EXPECT_THROW( std::rethrow_exception( result.exception() ), std::runtime_error );
}

TEST( ThrdLiteOutcomeFuture, GetNeverThrows )
{
    auto pair( make_outcome_promise_future<int>() );
    pair.first.run( []() -> int { throw std::runtime_error{ "boom" }; } );
    EXPECT_NO_THROW( { auto const result( pair.second.get() ); (void)result; } );
}

TEST( ThrdLiteOutcomeFuture, WaitThenGet )
{
    auto pair( make_outcome_promise_future<int>() );
    pair.first.set( 3 );
    pair.second.wait(); // idempotent -- must not consume/hang a second call
    auto const result( pair.second.get() );
    ASSERT_TRUE( result.has_value() );
    EXPECT_EQ( result.value(), 3 );
}

// No explicit wait()/get() at all -- the future's destructor must still block
// until the promise side completes (same lifetime contract as future.hpp's
// thrd_lite::future -- see its file-level note on why this has to be
// refcounted, not singly owned).
TEST( ThrdLiteOutcomeFuture, DestructorWaitsForCompletion )
{
    std::atomic<bool> completed{ false };
    {
        auto pair( make_outcome_promise_future<void>() );
        std::thread worker
        {
            [ promise = std::move( pair.first ), &completed ]() mutable noexcept
            {
                std::this_thread::sleep_for( std::chrono::milliseconds{ 20 } );
                completed.store( true, std::memory_order_release );
                promise.set( detail::outcome_ns::success() );
            }
        };
        worker.detach();
    }
    EXPECT_TRUE( completed.load( std::memory_order_acquire ) );
}

TEST( ThrdLiteOutcomeFuture, CrossThreadHandoff )
{
    auto pair( make_outcome_promise_future<int>() );
    std::thread worker
    {
        [ promise = std::move( pair.first ) ]() mutable noexcept
        {
            promise.run( []() noexcept { return 99; } );
        }
    };
    auto const result( pair.second.get() );
    ASSERT_TRUE( result.has_value() );
    EXPECT_EQ( result.value(), 99 );
    worker.join();
}

TEST( ThrdLiteOutcomeFuture, BrokenPromiseWhenDroppedIncomplete )
{
    auto pair( make_outcome_promise_future<int>() );
    { outcome_promise<int> dropped( std::move( pair.first ) ); } // destroyed without set()/run()
    auto const result( pair.second.get() );
    ASSERT_TRUE( result.has_exception() );
    EXPECT_THROW( std::rethrow_exception( result.exception() ), broken_promise );
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
