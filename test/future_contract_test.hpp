//==============================================================================
// Generic, Kind-parameterized contract tests shared by thrd_lite::future
// (future_test.cpp) and thrd_lite::outcome_future (outcome_future_test.cpp,
// only compiled with PSI_SWEATER_WITH_OUTCOME=ON) -- via gtest's TYPED_TEST_P/
// REGISTER_TYPED_TEST_SUITE_P/INSTANTIATE_TYPED_TEST_SUITE_P, the
// cross-translation-unit flavor of type-parameterized tests (plain
// TYPED_TEST_SUITE/TYPED_TEST needs every type in the list compiled into the
// SAME binary, which the optional outcome_future_test.cpp target rules out).
//
// A Kind supplies:
//   template <typename T> using future_type  = ...;
//   template <typename T> using promise_type = ...;
//   template <typename T> static std::pair<promise_type<T>, future_type<T>> make();
//   template <typename T> static T    get_value      ( future_type<T> & ); // asserts success, returns/discards T
//   template <typename T> static void expect_exception( future_type<T> & ); // asserts a failure state
// promise_type<T> itself must expose set_value(Args&&...), set_exception(std::exception_ptr),
// run(F&&) and wait() -- the shared surface thrd_lite::promise<T> and
// thrd_lite::outcome_promise<T> already agree on.
//==============================================================================
#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

template <typename Kind>
class FutureContract : public ::testing::Test {};

TYPED_TEST_SUITE_P( FutureContract );

TYPED_TEST_P( FutureContract, ValueSameThread )
{
    auto pair( TypeParam::template make<int>() );
    pair.first.set_value( 42 );
    EXPECT_EQ( TypeParam::get_value( pair.second ), 42 );
}

TYPED_TEST_P( FutureContract, VoidValueSameThread )
{
    auto pair( TypeParam::template make<void>() );
    pair.first.set_value();
    TypeParam::get_value( pair.second ); // must report success, not throw
}

TYPED_TEST_P( FutureContract, ExceptionSameThread )
{
    auto pair( TypeParam::template make<int>() );
    pair.first.set_exception( std::make_exception_ptr( std::runtime_error{ "boom" } ) );
    TypeParam::expect_exception( pair.second );
}

TYPED_TEST_P( FutureContract, RunCapturesResult )
{
    auto pair( TypeParam::template make<int>() );
    pair.first.run( []() noexcept { return 7; } );
    EXPECT_EQ( TypeParam::get_value( pair.second ), 7 );
}

TYPED_TEST_P( FutureContract, RunCapturesException )
{
    auto pair( TypeParam::template make<int>() );
    pair.first.run( []() -> int { throw std::runtime_error{ "boom" }; } );
    TypeParam::expect_exception( pair.second );
}

TYPED_TEST_P( FutureContract, WaitThenGet )
{
    auto pair( TypeParam::template make<int>() );
    pair.first.set_value( 3 );
    pair.second.wait(); // idempotent -- must not consume/hang a second call
    EXPECT_EQ( TypeParam::get_value( pair.second ), 3 );
}

// No explicit wait()/get() at all -- the future's destructor must still
// block until the promise side completes (the refcounted-slot lifetime --
// see future.hpp's file-level note -- applies to both Kinds identically).
TYPED_TEST_P( FutureContract, DestructorWaitsForCompletion )
{
    std::atomic<bool> completed{ false };
    {
        auto pair( TypeParam::template make<void>() );
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
    }
    EXPECT_TRUE( completed.load( std::memory_order_acquire ) );
}

TYPED_TEST_P( FutureContract, CrossThreadHandoff )
{
    auto pair( TypeParam::template make<int>() );
    std::thread worker
    {
        [ promise = std::move( pair.first ) ]() mutable noexcept
        {
            promise.run( []() noexcept { return 99; } );
        }
    };
    EXPECT_EQ( TypeParam::get_value( pair.second ), 99 );
    worker.join();
}

TYPED_TEST_P( FutureContract, MoveOnlyValueType )
{
    auto pair( TypeParam::template make<std::unique_ptr<int>>() );
    pair.first.set_value( std::make_unique<int>( 5 ) );
    auto p( TypeParam::get_value( pair.second ) );
    ASSERT_NE( p, nullptr );
    EXPECT_EQ( *p, 5 );
}

TYPED_TEST_P( FutureContract, BrokenPromiseWhenDroppedIncomplete )
{
    auto pair( TypeParam::template make<int>() );
    { typename TypeParam::template promise_type<int> dropped( std::move( pair.first ) ); } // destroyed without set_value()/set_exception()/run()
    TypeParam::expect_exception( pair.second );
}

REGISTER_TYPED_TEST_SUITE_P
(
    FutureContract,
    ValueSameThread, VoidValueSameThread, ExceptionSameThread,
    RunCapturesResult, RunCapturesException, WaitThenGet,
    DestructorWaitsForCompletion, CrossThreadHandoff, MoveOnlyValueType,
    BrokenPromiseWhenDroppedIncomplete
);

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
