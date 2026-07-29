//==============================================================================
// Tests for the standalone psi::thrd_lite::promise/future pair (future.hpp) --
// independent of any sweater shop/dispatch() usage. Covers the value/void/
// exception result paths, the wait()-then-get() and destructor-waits (no
// explicit wait()/get() call at all) completion paths, and a cross-thread
// producer/consumer hand-off.
//==============================================================================

#include <psi/sweater/threading/future.hpp>

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

TEST( ThrdLiteFuture, ValueSameThread )
{
    auto pair( make_promise_future<int>() );
    pair.first.set_value( 42 );
    EXPECT_EQ( pair.second.get(), 42 );
}

TEST( ThrdLiteFuture, VoidValueSameThread )
{
    auto pair( make_promise_future<void>() );
    pair.first.set_value();
    pair.second.get(); // must not throw
}

TEST( ThrdLiteFuture, ExceptionSameThread )
{
    auto pair( make_promise_future<int>() );
    pair.first.set_exception( std::make_exception_ptr( std::runtime_error{ "boom" } ) );
    EXPECT_THROW( pair.second.get(), std::runtime_error );
}

TEST( ThrdLiteFuture, RunCapturesResult )
{
    auto pair( make_promise_future<int>() );
    pair.first.run( []() noexcept { return 7; } );
    EXPECT_EQ( pair.second.get(), 7 );
}

TEST( ThrdLiteFuture, RunCapturesException )
{
    auto pair( make_promise_future<int>() );
    pair.first.run( []() -> int { throw std::runtime_error{ "boom" }; } );
    EXPECT_THROW( pair.second.get(), std::runtime_error );
}

TEST( ThrdLiteFuture, WaitThenGet )
{
    auto pair( make_promise_future<int>() );
    pair.first.set_value( 3 );
    pair.second.wait(); // idempotent -- must not consume/hang a second call
    EXPECT_EQ( pair.second.get(), 3 );
}

// No explicit wait()/get() at all -- the future's destructor must still block
// until the promise side completes, never leaving the slot dangling under the
// (still in-flight) worker thread.
TEST( ThrdLiteFuture, DestructorWaitsForCompletion )
{
    std::atomic<bool> completed{ false };
    {
        auto pair( make_promise_future<void>() );
        std::thread worker
        {
            [ promise = std::move( pair.first ), &completed ]() mutable noexcept
            {
                std::this_thread::sleep_for( std::chrono::milliseconds{ 20 } );
                completed.store( true, std::memory_order_release );
                promise.set_value();
            }
        };
        worker.detach();
        // pair.second (the future) goes out of scope here -- its destructor
        // must wait for the detached worker's set_value() before returning.
    }
    EXPECT_TRUE( completed.load( std::memory_order_acquire ) );
}

TEST( ThrdLiteFuture, CrossThreadHandoff )
{
    auto pair( make_promise_future<int>() );
    std::thread worker
    {
        [ promise = std::move( pair.first ) ]() mutable noexcept
        {
            promise.run( []() noexcept { return 99; } );
        }
    };
    EXPECT_EQ( pair.second.get(), 99 );
    worker.join();
}

TEST( ThrdLiteFuture, BrokenPromiseWhenDroppedIncomplete )
{
    auto pair( make_promise_future<int>() );
    { promise<int> dropped( std::move( pair.first ) ); } // destroyed without set_value()/set_exception()/run()
    EXPECT_THROW( pair.second.get(), broken_promise );
}

TEST( ThrdLiteFuture, MoveOnlyValueType )
{
    auto pair( make_promise_future<std::unique_ptr<int>>() );
    pair.first.set_value( std::make_unique<int>( 5 ) );
    auto p( pair.second.get() );
    ASSERT_NE( p, nullptr );
    EXPECT_EQ( *p, 5 );
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
