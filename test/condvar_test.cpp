//==============================================================================
// psi::thrd_lite::condition_variable (condvar.hpp): the timed wait (false =
// timed out) and the predicate-loop wait_until() built on top of it.
//==============================================================================

#include <psi/sweater/threading/condvar.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <utility>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

using namespace std::chrono_literals;
using steady   = std::chrono::steady_clock;
using lock_t   = condition_variable::lock_t;

// Slack for timers that may fire a scheduler tick early (Windows).
constexpr auto timer_slack{ 20ms };

TEST( CondVar, TimedWaitTimesOut )
{
    condition_variable cv;
    condition_variable::mutex_t m;
    lock_t lock{ m };

    auto const start{ steady::now() };
    // A spurious wakeup is allowed to return true early; loop until a timeout is reported.
    while ( cv.wait( lock, 50ms ) ) {}
    EXPECT_GE( steady::now() - start, 50ms - timer_slack );
    EXPECT_TRUE( lock.owns_lock() );
}

TEST( CondVar, NonPositiveTimeoutReturnsPromptly )
{
    condition_variable cv;
    condition_variable::mutex_t m;
    lock_t lock{ m };

    auto const start{ steady::now() };
    EXPECT_FALSE( cv.wait( lock,  0ms ) );
    EXPECT_FALSE( cv.wait( lock, -5ms ) );
    EXPECT_LT( steady::now() - start, 1s );
}

TEST( CondVar, TimedWaitIsWokenByNotify )
{
    condition_variable cv;
    condition_variable::mutex_t m;
    bool ready{ false };

    std::thread notifier{ [ & ] {
        std::this_thread::sleep_for( 20ms );
        { lock_t const lock{ m }; ready = true; }
        cv.notify_one();
    } };

    lock_t lock{ m };
    auto const start{ steady::now() };
    while ( !ready )
        ASSERT_TRUE( cv.wait( lock, 10s ) ) << "timed out instead of being notified";
    EXPECT_LT( steady::now() - start, 5s );
    lock.unlock();
    notifier.join();
}

TEST( CondVar, WaitUntilReturnsTrueWhenPredicateBecomesTrue )
{
    condition_variable cv;
    condition_variable::mutex_t m;
    bool ready{ false };

    std::thread notifier{ [ & ] {
        std::this_thread::sleep_for( 20ms );
        { lock_t const lock{ m }; ready = true; }
        cv.notify_all();
    } };

    lock_t lock{ m };
    EXPECT_TRUE( cv.wait_until( lock, steady::now() + 10s, [ & ] { return ready; } ) );
    lock.unlock();
    notifier.join();
}

TEST( CondVar, WaitUntilTimesOutWithFalsePredicate )
{
    condition_variable cv;
    condition_variable::mutex_t m;
    lock_t lock{ m };

    auto const deadline{ steady::now() + 50ms };
    EXPECT_FALSE( cv.wait_until( lock, deadline, [] { return false; } ) );
    EXPECT_GE( steady::now(), deadline );
    EXPECT_TRUE( lock.owns_lock() );
}

TEST( CondVar, WaitUntilPastDeadlineChecksPredicateOnce )
{
    condition_variable cv;
    condition_variable::mutex_t m;
    lock_t lock{ m };

    auto const past{ steady::now() - 1s };
    int calls{ 0 };
    EXPECT_TRUE ( cv.wait_until( lock, past, [ & ] { ++calls; return true ; } ) );
    EXPECT_FALSE( cv.wait_until( lock, past, [ & ] { ++calls; return false; } ) );
    EXPECT_EQ( calls, 2 );
}

TEST( CondVar, MovedFromAndMovedToBothWait )
{
    condition_variable source;
    condition_variable target{ std::move( source ) };
    condition_variable::mutex_t m;
    lock_t lock{ m };
    while ( source.wait( lock, 1ms ) ) {}
    while ( target.wait( lock, 1ms ) ) {}
}

TEST( CondVar, MoveLeavesTheSourceIntact )
{
    // The move constructs a fresh condvar: a source with a waiter keeps its waiter.
    condition_variable source;
    condition_variable::mutex_t m;
    bool ready{ false };
    std::thread waiter{ [ & ] {
        lock_t lock{ m };
        while ( !ready )
            source.wait( lock );
    } };
    std::this_thread::sleep_for( 20ms );
    {
        condition_variable target{ std::move( source ) }; // moving and destroying must not disturb the waiter
    }
    { lock_t const lock{ m }; ready = true; }
    source.notify_all();
    waiter.join();
}

TEST( CondVar, OverlongTimeoutIsClamped )
{
    EXPECT_EQ( clamp_timeout_ms( -5ms                                    ), 0u          );
    EXPECT_EQ( clamp_timeout_ms( 1500ms                                  ), 1500u       );
    EXPECT_EQ( clamp_timeout_ms( std::chrono::milliseconds::max()        ), 0xFFFF'FFFEu );

    condition_variable cv;
    condition_variable::mutex_t m;
    lock_t lock{ m };
    std::thread notifier{ [ & ] {
        std::this_thread::sleep_for( 20ms );
        lock_t const l{ m };
        cv.notify_one();
    } };
    cv.wait( lock, std::chrono::milliseconds::max() ); // must neither overflow nor return an error
    notifier.join();
}

#if !defined( _WIN32 ) && !defined( PSI_THRD_LITE_CONDVAR_MONOTONIC_ATTR )
// Global/static condvars must not need dynamic initialisation.
constinit condition_variable static_cv;
#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
