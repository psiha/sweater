//==============================================================================
// Adversarial/stress coverage for psi::sweater::shop's own dispatch/thread-pool
// machinery -- sweater_smoke_test only exercises three single-shot basic-usage
// cases (sum/future/fire-and-forget) and says nothing about shutdown races,
// concurrent-producer contention, or the actual (process-wide, not per-shop)
// semantics of in_flight_count()/wait_until_idle(). See README.md's Testing
// section ("Known gap") for the origin of this file.
//==============================================================================

#include <psi/sweater/sweater.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

//------------------------------------------------------------------------------
namespace psi::sweater
{
//------------------------------------------------------------------------------

namespace
{
    auto const stress_deadline{ std::chrono::seconds{ 10 } };
} // anonymous namespace

// Burst-enqueue a batch of fire_and_forget work, then confirm it all
// completes via wait_until_idle() before the shop goes out of scope.
//
// This was originally written assuming shop::~shop() itself guarantees a
// full drain (true for the generic/Linux worker-pool backend, whose
// worker_loop_impl drains its queue to empty before honoring the shutdown
// flag) -- but that is NOT a cross-platform guarantee: windows.hpp's and
// apple.hpp's shop are explicitly *stateless* and submit fire_and_forget
// work to the OS's shared, process-wide thread pool (Windows Thread Pool
// API / GCD's global dispatch queue) with no per-shop thread to join on
// destruction. Destroying a shop there does not wait for anything --
// confirmed empirically (a burst of 2000 items regularly lost 10-60% of
// completions on Windows, with some `completed` counts exceeding
// items_per_iteration entirely, because leftover callbacks from a
// *previous*, already-"destroyed" iteration's shop were still running and
// writing into a reused stack slot). wait_until_idle() is the actual,
// documented, portable way to know fire_and_forget work has finished; this
// test now pins that down instead of an implementation detail of one
// backend.
TEST( SweatShopStress, BurstEnqueueThenWaitDrainsAllWork )
{
    auto constexpr iterations{ 25 };
    auto constexpr items_per_iteration{ 2000 };

    for ( auto iter{ 0 }; iter < iterations; ++iter )
    {
        std::atomic<int> completed{ 0 };
        {
            shop work_shop;
            for ( auto i{ 0 }; i < items_per_iteration; ++i )
            {
                work_shop.fire_and_forget( [&]() noexcept { completed.fetch_add( 1, std::memory_order_relaxed ); } );
            }
            ASSERT_TRUE( wait_until_idle( stress_deadline ) )
                << "iteration " << iter << ": burst-enqueued work did not drain within the stress deadline";
        }
        EXPECT_EQ( completed.load(), items_per_iteration )
            << "iteration " << iter << ": burst-enqueued work was not fully completed";
    }
}

// Many threads simultaneously hammering fire_and_forget/dispatch/spread_the_sweat
// on one shared shop. Nothing in sweater_smoke_test exercises concurrent
// producers at all (each of its three cases enqueues from a single thread).
TEST( SweatShopStress, ConcurrentProducersOnSharedShop )
{
    auto const producer_count{ std::max( 4u, std::thread::hardware_concurrency() ) };
    auto constexpr ops_per_producer{ 501 }; // multiple of 3 so i % 3 splits evenly across the three kinds below

    shop work_shop;
    std::atomic<std::size_t> fire_and_forget_count{ 0 };
    std::atomic<std::size_t> dispatch_count        { 0 };
    std::atomic<std::size_t> spread_count          { 0 };

    std::vector<std::thread> producers;
    producers.reserve( producer_count );
    for ( auto p{ 0u }; p < producer_count; ++p )
    {
        producers.emplace_back( [&]
        {
            for ( auto i{ 0 }; i < ops_per_producer; ++i )
            {
                switch ( i % 3 )
                {
                    case 0:
                        work_shop.fire_and_forget( [&]() noexcept { fire_and_forget_count.fetch_add( 1, std::memory_order_relaxed ); } );
                        break;
                    case 1:
                        // Alternate dispatch()/dispatch_lite() -- both funnel into
                        // dispatch_count the same way, so this adds concurrent-
                        // producer stress coverage for the leaner completion path
                        // without perturbing the per-kind count assertions below.
                        if ( i % 2 == 0 )
                            work_shop.dispatch     ( [&]() noexcept -> int { dispatch_count.fetch_add( 1, std::memory_order_relaxed ); return 0; } ).wait();
                        else
                            work_shop.dispatch_lite( [&]() noexcept -> int { dispatch_count.fetch_add( 1, std::memory_order_relaxed ); return 0; } ).wait();
                        break;
                    case 2:
                        work_shop.spread_the_sweat( 8, [&]( auto const start, auto const end ) noexcept
                        {
                            spread_count.fetch_add( static_cast<std::size_t>( end - start ), std::memory_order_relaxed );
                        } );
                        break;
                }
            }
        } );
    }
    for ( auto & producer : producers )
    {
        producer.join();
    }

    ASSERT_TRUE( wait_until_idle( stress_deadline ) ) << "sweat_shop work did not drain within the stress deadline";

    auto const expected_per_kind{ static_cast<std::size_t>( producer_count ) * ( ops_per_producer / 3 ) };
    EXPECT_EQ( fire_and_forget_count.load(), expected_per_kind );
    EXPECT_EQ( dispatch_count        .load(), expected_per_kind );
    EXPECT_EQ( spread_count          .load(), expected_per_kind * 8 );
}

// psi::sweater::detail::g_in_flight (dispatch_tracking.hpp) is a SINGLE
// process-wide atomic counter, not one per shop. This means wait_until_idle()
// called while only caring about one shop's work can still observe -- and
// block on -- a completely unrelated shop's in-flight work. This is surprising
// and easy to miss (there is no per-shop overload to reach for instead); pin it
// down explicitly rather than leaving it as an undocumented, untested behavior.
TEST( SweatShopStress, InFlightTrackingIsProcessWideNotPerShop )
{
    std::mutex              release_mutex;
    std::condition_variable release_cv;
    bool                    release{ false };
    std::atomic<bool>       busy_task_started{ false };

    shop idle_shop; // never given any work
    shop busy_shop; // holds one long-running in-flight task throughout

    busy_shop.fire_and_forget( [&]() noexcept
    {
        busy_task_started.store( true, std::memory_order_release );
        std::unique_lock lock{ release_mutex };
        release_cv.wait( lock, [&] { return release; } );
    } );

    while ( !busy_task_started.load( std::memory_order_acquire ) )
    {
        std::this_thread::yield();
    }

    // idle_shop has no work of its own, yet the process-wide tracker still
    // reports work in flight (busy_shop's blocked task) and a short-timeout
    // wait_until_idle() must NOT succeed while it's still parked.
    EXPECT_GT( in_flight_count(), 0u );
    EXPECT_FALSE( wait_until_idle( std::chrono::milliseconds{ 200 } ) )
        << "wait_until_idle() returned true while an unrelated shop's task was still in flight";

    {
        std::scoped_lock lock{ release_mutex };
        release = true;
    }
    release_cv.notify_one();

    EXPECT_TRUE( wait_until_idle( stress_deadline ) );
    (void)idle_shop;
}

namespace
{
    template <typename Predicate>
    bool wait_for( Predicate const & predicate, std::chrono::steady_clock::duration const timeout )
    {
        auto const deadline{ std::chrono::steady_clock::now() + timeout };
        while ( !predicate() )
        {
            if ( std::chrono::steady_clock::now() >= deadline )
                return false;
            std::this_thread::sleep_for( std::chrono::milliseconds{ 1 } );
        }
        return true;
    }

    // Occupies `blockers` workers with tasks that block until released, each
    // fired only after the previous one has started (so every blocker holds a
    // distinct worker - and each of them has to get past the ones already
    // blocked), then fires `probes` short tasks: while at least one worker is
    // still free they must all complete without the blockers being released. The release flag is raised on every exit path before the shop
    // is destroyed (its destructor joins workers that would otherwise never
    // finish their blocked task).
    void probes_complete_while_workers_are_blocked( unsigned const blockers, unsigned const probes )
    {
        auto const let_idle_workers_fall_asleep{ [] { std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } ); } };

        shop work_shop;
        std::atomic<bool    > release{ false };
        std::atomic<unsigned> started{ 0     };
        std::atomic<unsigned> probed { 0     };
        struct release_guard
        {
            std::atomic<bool> & flag;
            ~release_guard() { flag.store( true, std::memory_order_release ); flag.notify_all(); }
        } const guard{ release };

        // Workers still starting up would pick the blockers up regardless of
        // where they were queued.
        let_idle_workers_fall_asleep();

        for ( auto b{ 0u }; b < blockers; ++b )
        {
            work_shop.fire_and_forget( [&]() noexcept
            {
                started.fetch_add( 1, std::memory_order_release );
                release.wait( false, std::memory_order_acquire );
            } );
            ASSERT_TRUE( wait_for( [&] { return started.load( std::memory_order_acquire ) == b + 1; }, std::chrono::seconds{ 2 } ) )
                << "blocker " << b << " did not start while " << work_shop.number_of_workers() - b << " workers were free";
        }

        // Workers woken along with the blockers would otherwise pick the probes
        // up by chance while still looking for work.
        let_idle_workers_fall_asleep();

        for ( auto p{ 0u }; p < probes; ++p )
        {
            work_shop.fire_and_forget( [&]() noexcept { probed.fetch_add( 1, std::memory_order_release ); } );
        }
        EXPECT_TRUE( wait_for( [&] { return probed.load( std::memory_order_acquire ) == probes; }, std::chrono::seconds{ 2 } ) )
            << probes - probed.load() << " of " << probes << " short tasks stayed queued behind " << blockers << " blocked task(s) despite idle workers";
    }

    // The worker threads that execute fire_and_forget work (the generic pool
    // counts the calling thread, which only takes part in spread_the_sweat).
    unsigned fire_workers( shop const & work_shop ) noexcept
    {
#   ifdef PSI_SWEATER_USE_CALLER_THREAD
        unsigned const caller_thread{ PSI_SWEATER_USE_CALLER_THREAD };
#   else
        unsigned const caller_thread{ 0 };
#   endif
        return static_cast<unsigned>( work_shop.number_of_workers() ) - caller_thread;
    }
} // anonymous namespace

// A long-running task must not hold back work fired after it while other
// workers sit idle.
TEST( SweatShopStress, IdleWorkerRunsWorkFiredBehindABlockedTask )
{
    shop const probe_shop;
    if ( fire_workers( probe_shop ) < 2 )
        GTEST_SKIP() << "needs at least two worker threads";
    probes_complete_while_workers_are_blocked( 1, 3 );
}

// Same with all but one worker blocked: the remaining worker has to be found.
TEST( SweatShopStress, LastIdleWorkerRunsWorkFiredBehindBlockedTasks )
{
    shop const probe_shop;
    auto const workers{ fire_workers( probe_shop ) };
    if ( workers < 2 )
        GTEST_SKIP() << "needs at least two worker threads";
    probes_complete_while_workers_are_blocked( workers - 1, 3 );
}

//------------------------------------------------------------------------------
} // namespace psi::sweater
//------------------------------------------------------------------------------
