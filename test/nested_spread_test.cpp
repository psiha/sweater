//==============================================================================
// spread_the_sweat called while the generic pool already has work: from inside
// work running on one of its workers (a recursive spread), and from outside
// while every worker is taken. In both cases the caller must make progress by
// itself (it can never wait for work queued behind the very workers that are
// busy), free workers must join in, and every iteration must run exactly once.
//
// The generic pool is exercised directly: the OS-provided pools have a
// different (kernel-managed) threading model and nothing here applies to them.
//==============================================================================

#include <psi/sweater/impls/generic.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

//------------------------------------------------------------------------------
namespace psi::sweater::generic
{
//------------------------------------------------------------------------------

namespace
{
    using steady = std::chrono::steady_clock;

    auto const patience{ std::chrono::seconds{ 20 } };

    template <typename Predicate>
    bool wait_for( Predicate const & predicate, steady::duration const timeout = patience )
    {
        auto const deadline{ steady::now() + timeout };
        while ( !predicate() )
        {
            if ( steady::now() >= deadline )
                return false;
            std::this_thread::sleep_for( std::chrono::microseconds{ 200 } );
        }
        return true;
    }

    // The worker threads of the pool (the calling thread of a spread is counted
    // as a worker by number_of_workers() where it takes part).
    unsigned worker_threads( shop const & work_shop ) noexcept
    {
        return static_cast<unsigned>( work_shop.number_of_workers() ) - unsigned{ PSI_SWEATER_USE_CALLER_THREAD };
    }

    bool settles( shop const & work_shop ) { return wait_for( [&] { return work_shop.number_of_items() == 0; } ); }

    // Counts how often each iteration of a spread was run.
    class iteration_counts
    {
    public:
        explicit iteration_counts( std::uint32_t const iterations ) : counts_{ std::make_unique<std::atomic<std::uint32_t>[]>( iterations ) }, size_{ iterations } {}

        void ran( std::uint32_t const start, std::uint32_t const end ) noexcept
        {
            for ( auto i{ start }; i < end; ++i )
                counts_[ i ].fetch_add( 1, std::memory_order_relaxed );
        }

        std::uint32_t not_run_exactly_once() const noexcept
        {
            std::uint32_t wrong{ 0 };
            for ( std::uint32_t i{ 0 }; i < size_; ++i )
                wrong += ( counts_[ i ].load( std::memory_order_relaxed ) != 1 );
            return wrong;
        }

    private:
        std::unique_ptr<std::atomic<std::uint32_t>[]> counts_;
        std::uint32_t                                 size_  ;
    }; // class iteration_counts

    class thread_set
    {
    public:
        void add_current() noexcept
        {
            std::scoped_lock const lock{ mutex_ };
            threads_.insert( std::this_thread::get_id() );
            size_.store( static_cast<unsigned>( threads_.size() ), std::memory_order_release );
        }
        unsigned size() const noexcept { return size_.load( std::memory_order_acquire ); }
        bool     only( std::thread::id const thread ) const
        {
            std::scoped_lock const lock{ mutex_ };
            return ( threads_.size() == 1 ) && ( *threads_.begin() == thread );
        }

    private:
        mutable std::mutex        mutex_  ;
        std::set<std::thread::id> threads_;
        std::atomic<unsigned>     size_{ 0 };
    }; // class thread_set

    // Takes every thread of the pool - its workers and the calling thread -
    // except for spare_workers of the workers, with one iteration each of an
    // outer spread, all of them held until told otherwise. Then runs on_worker
    // on one of the held workers and on_outsider on a thread that is not part
    // of the pool, and lets go once both are done.
    //
    // The outer spread does not promise a thread per iteration (a worker may
    // go back to sleep while a part it could have taken is still queued behind
    // a held one), so an attempt that does not get every thread is given up
    // and repeated.
    enum class attempt { done, pool_not_taken, failed };

    template <typename OnWorker, typename OnOutsider>
    attempt try_with_workers_taken( shop & work_shop, std::uint32_t const spare_workers, OnWorker const & on_worker, OnOutsider const & on_outsider )
    {
        auto const everyone    { static_cast<std::uint32_t>( work_shop.number_of_workers() ) - spare_workers };
        auto const outer_caller{ std::this_thread::get_id() };
        enum : int { undecided, taken, given_up };
        std::atomic<std::uint32_t> held         { 0         };
        std::atomic<bool         > shared_thread{ false     };
        std::atomic<int          > pool         { undecided };
        std::atomic<bool         > worker_picked{ false     };
        std::atomic<bool         > worker_done  { false     };
        std::atomic<bool         > outsider_done{ false     };
        std::atomic<bool         > failed       { false     };

        std::thread outsider
        {
            [&]
            {
                auto const everyone_held{ wait_for( [&] { return held.load( std::memory_order_acquire ) == everyone; }, std::chrono::milliseconds{ 500 } ) };
                if ( everyone_held && !shared_thread.load( std::memory_order_acquire ) )
                {
                    pool.store( taken, std::memory_order_release );
                    on_outsider();
                    outsider_done.store( true, std::memory_order_release );
                }
                else
                {
                    pool.store( given_up, std::memory_order_release );
                }
            }
        };

        work_shop.spread_the_sweat
        (
            everyone,
            [&]( std::uint32_t const start, std::uint32_t const end ) noexcept
            {
                if ( ( end - start ) != 1 )
                    shared_thread.store( true, std::memory_order_release );
                held.fetch_add( end - start, std::memory_order_release );
                if ( !wait_for( [&] { return pool.load( std::memory_order_acquire ) != undecided; } ) )
                    failed.store( true, std::memory_order_release );
                if ( pool.load( std::memory_order_acquire ) != taken )
                    return;
                if ( ( std::this_thread::get_id() != outer_caller ) && !worker_picked.exchange( true, std::memory_order_acq_rel ) )
                {
                    on_worker();
                    worker_done.store( true, std::memory_order_release );
                }
                auto const both_done{ [&] { return worker_done.load( std::memory_order_acquire ) && outsider_done.load( std::memory_order_acquire ); } };
                if ( !wait_for( [&] { return both_done() || failed.load( std::memory_order_acquire ); } ) )
                    failed.store( true, std::memory_order_release );
            }
        );
        outsider.join();
        if ( failed.load( std::memory_order_acquire ) )
            return attempt::failed;
        return ( pool.load( std::memory_order_acquire ) == taken ) ? attempt::done : attempt::pool_not_taken;
    }

    // False if the pool could not be taken at all or something did not finish
    // in time.
    // How often the pool could not be taken at the first attempt (printed at
    // the end of a run).
    std::atomic<unsigned> repeated_attempts{ 0 };

    template <typename OnWorker, typename OnOutsider>
    bool with_workers_taken( shop & work_shop, std::uint32_t const spare_workers, OnWorker const & on_worker, OnOutsider const & on_outsider )
    {
        for ( auto tries{ 0 }; tries < 100; ++tries )
        {
            if ( !settles( work_shop ) )
                return false;
            auto const result{ try_with_workers_taken( work_shop, spare_workers, on_worker, on_outsider ) };
            if ( result != attempt::pool_not_taken )
                return result == attempt::done;
            repeated_attempts.fetch_add( 1, std::memory_order_relaxed );
        }
        return false;
    }

    template <typename OnWorker, typename OnOutsider>
    bool with_every_worker_taken( shop & work_shop, OnWorker const & on_worker, OnOutsider const & on_outsider )
    {
        return with_workers_taken( work_shop, 0, on_worker, on_outsider );
    }

    class repeated_attempts_report : public ::testing::Environment
    {
        void TearDown() override { std::printf( "attempts at taking the pool that had to be repeated: %u\n", repeated_attempts.load() ); }
    }; // class repeated_attempts_report
    [[ maybe_unused ]] auto const * const report{ ::testing::AddGlobalTestEnvironment( new repeated_attempts_report ) };

    // Runs work on one of the pool's workers and waits for it.
    template <typename Work>
    bool run_on_a_worker( shop & work_shop, Work const & work )
    {
        std::atomic<bool> done{ false };
        work_shop.fire_and_forget( [&]() noexcept { work(); done.store( true, std::memory_order_release ); } );
        return wait_for( [&] { return done.load( std::memory_order_acquire ); } );
    }
} // anonymous namespace

// With a worker to spare, a spread started from inside work running on the pool
// is not run by the calling worker alone.
TEST( NestedSpread, FreeWorkersJoinASpreadStartedFromAWorker )
{
    shop work_shop;
    if ( worker_threads( work_shop ) < 2 )
        GTEST_SKIP() << "needs at least two worker threads";

    auto const iterations{ static_cast<std::uint32_t>( work_shop.number_of_workers() ) * 64 };
    for ( auto round{ 0 }; round < 20; ++round )
    {
        iteration_counts counts{ iterations };
        thread_set       threads;
        ASSERT_TRUE( settles( work_shop ) );
        ASSERT_TRUE
        (
            run_on_a_worker( work_shop, [&]() noexcept
            {
                work_shop.spread_the_sweat( iterations, [&]( std::uint32_t const start, std::uint32_t const end ) noexcept
                {
                    threads.add_current();
                    // Hold the first pieces until somebody else turned up, so
                    // that the calling worker cannot simply be done before the
                    // others had a chance to wake.
                    wait_for( [&] { return threads.size() > 1; }, std::chrono::seconds{ 2 } );
                    counts.ran( start, end );
                } );
            } )
        );
        EXPECT_GT( threads.size(), 1u ) << "the calling worker ran the whole spread while the other workers were free";
        EXPECT_EQ( counts.not_run_exactly_once(), 0u );
    }
    EXPECT_TRUE( settles( work_shop ) ) << work_shop.number_of_items() << " items left in the shop";
}

// Every thread of the pool spreads at the same moment, from inside an outer
// spread that occupies all of them: nobody may wait for anybody else's queued
// work, and nothing may be run twice or left out.
TEST( NestedSpread, EveryWorkerSpreadingAtOnceCompletes )
{
    shop work_shop;
    auto const everyone  { static_cast<std::uint32_t>( work_shop.number_of_workers() ) };
    auto const iterations{ std::uint32_t{ 1000 } };

    for ( auto round{ 0 }; round < 50; ++round )
    {
        std::vector<iteration_counts> counts;
        counts.reserve( everyone );
        for ( std::uint32_t i{ 0 }; i < everyone; ++i )
            counts.emplace_back( iterations );

        std::atomic<std::uint32_t> at_the_gate{ 0 };
        std::atomic<bool>          finished   { false };
        std::thread spreader
        {
            [&]
            {
                work_shop.spread_the_sweat( everyone, [&]( std::uint32_t const outer_start, std::uint32_t const outer_end ) noexcept
                {
                    // Best effort at making the inner spreads start together.
                    at_the_gate.fetch_add( outer_end - outer_start, std::memory_order_release );
                    wait_for( [&] { return at_the_gate.load( std::memory_order_acquire ) == everyone; }, std::chrono::milliseconds{ 20 } );
                    for ( auto outer{ outer_start }; outer < outer_end; ++outer )
                    {
                        work_shop.spread_the_sweat( iterations, [&, outer]( std::uint32_t const start, std::uint32_t const end ) noexcept
                        {
                            counts[ outer ].ran( start, end );
                        } );
                    }
                } );
                finished.store( true, std::memory_order_release );
            }
        };
        if ( !wait_for( [&] { return finished.load( std::memory_order_acquire ); } ) )
        {
            ADD_FAILURE() << "spreads started from every worker at once did not complete (round " << round << ")";
            std::abort(); // the threads are stuck: nothing to join
        }
        spreader.join();
        for ( std::uint32_t i{ 0 }; i < everyone; ++i )
            ASSERT_EQ( counts[ i ].not_run_exactly_once(), 0u ) << "inner spread " << i << ", round " << round;
    }
    EXPECT_TRUE( settles( work_shop ) ) << work_shop.number_of_items() << " items left in the shop";
}

// Many items running on the pool spread concurrently, over and over.
TEST( NestedSpread, ConcurrentSpreadsFromFiredWorkRunEveryIterationOnce )
{
    shop work_shop;
    if ( worker_threads( work_shop ) < 1 )
        GTEST_SKIP() << "needs a worker thread";

    auto const tasks     { static_cast<std::uint32_t>( work_shop.number_of_workers() ) * 2 };
    auto const iterations{ std::uint32_t{ 257 } };

    for ( auto round{ 0 }; round < 100; ++round )
    {
        std::vector<iteration_counts> counts;
        counts.reserve( tasks );
        for ( std::uint32_t i{ 0 }; i < tasks; ++i )
            counts.emplace_back( iterations );

        std::atomic<std::uint32_t> done{ 0 };
        for ( std::uint32_t task{ 0 }; task < tasks; ++task )
        {
            work_shop.fire_and_forget( [&, task]() noexcept
            {
                work_shop.spread_the_sweat( iterations, [&, task]( std::uint32_t const start, std::uint32_t const end ) noexcept
                {
                    counts[ task ].ran( start, end );
                } );
                done.fetch_add( 1, std::memory_order_release );
            } );
        }
        ASSERT_TRUE( wait_for( [&] { return done.load( std::memory_order_acquire ) == tasks; } ) ) << "round " << round;
        for ( std::uint32_t i{ 0 }; i < tasks; ++i )
            ASSERT_EQ( counts[ i ].not_run_exactly_once(), 0u ) << "task " << i << ", round " << round;
    }
    EXPECT_TRUE( settles( work_shop ) ) << work_shop.number_of_items() << " items left in the shop";
}

// With every other thread of the pool held, a spread started from a worker is
// run by that worker alone and returns while the others are still held.
TEST( NestedSpread, AWorkerSpreadsAloneWhenThePoolIsBusy )
{
    shop work_shop;
    if ( worker_threads( work_shop ) < 1 )
        GTEST_SKIP() << "needs a worker thread";

    auto const       iterations{ std::uint32_t{ 4096 } };
    iteration_counts counts{ iterations };
    thread_set       threads;
    std::thread::id  spreading_worker;
    ASSERT_TRUE( settles( work_shop ) );
    ASSERT_TRUE
    (
        with_every_worker_taken
        (
            work_shop,
            [&]() noexcept
            {
                spreading_worker = std::this_thread::get_id();
                work_shop.spread_the_sweat( iterations, [&]( std::uint32_t const start, std::uint32_t const end ) noexcept
                {
                    threads.add_current();
                    counts.ran( start, end );
                } );
            },
            [] {}
        )
    );
    EXPECT_TRUE( threads.only( spreading_worker ) );
    EXPECT_EQ  ( counts.not_run_exactly_once(), 0u );
    EXPECT_TRUE( settles( work_shop ) ) << work_shop.number_of_items() << " items left in the shop";
}

// One worker is free and all the others are held, whichever they are: a spread
// started from one of the held workers is joined by the free one.
TEST( NestedSpread, AFreeWorkerJoinsWhileTheOthersAreHeld )
{
    shop work_shop;
    if ( worker_threads( work_shop ) < 3 )
        GTEST_SKIP() << "needs at least three worker threads";

    for ( auto round{ 0 }; round < 10; ++round )
    {
        auto const       iterations{ std::uint32_t{ 4096 } };
        iteration_counts counts{ iterations };
        thread_set       threads;
        ASSERT_TRUE
        (
            with_workers_taken
            (
                work_shop,
                1,
                [&]() noexcept
                {
                    work_shop.spread_the_sweat( iterations, [&]( std::uint32_t const start, std::uint32_t const end ) noexcept
                    {
                        threads.add_current();
                        wait_for( [&] { return threads.size() > 1; }, std::chrono::seconds{ 2 } );
                        counts.ran( start, end );
                    } );
                },
                [] {}
            )
        ) << "round " << round;
        EXPECT_EQ( threads.size(), 2u ) << "round " << round;
        EXPECT_EQ( counts.not_run_exactly_once(), 0u );
    }
    EXPECT_TRUE( settles( work_shop ) ) << work_shop.number_of_items() << " items left in the shop";
}

// Spreads from a thread outside the pool run while work on the pool keeps
// spreading: whoever ends up running whose pieces, every spread runs each of
// its iterations exactly once.
TEST( NestedSpread, SpreadsFromOutsideAndFromWorkersMix )
{
    shop work_shop;
    if ( worker_threads( work_shop ) < 1 )
        GTEST_SKIP() << "needs a worker thread";

    auto const iterations     { std::uint32_t{ 512 } };
    auto const spreaders      { std::min( worker_threads( work_shop ), 2u ) };
    auto const outside_spreads{ 2000 };
    // A spreader also stops by itself: a caller that waits for its own spread
    // takes items from the queue, so a spreader may end up running on this
    // thread, which then is not there to tell it to stop.
    auto const nested_limit   { 20000 };

    std::atomic<bool>          stop          { false };
    std::atomic<unsigned>      spreading     { spreaders };
    std::atomic<std::uint64_t> nested_spreads{ 0 };
    std::atomic<std::uint64_t> wrong         { 0 };
    for ( auto spreader{ 0u }; spreader < spreaders; ++spreader )
    {
        work_shop.fire_and_forget( [&]() noexcept
        {
            for ( auto spread{ 0 }; ( spread < nested_limit ) && !stop.load( std::memory_order_acquire ); ++spread )
            {
                iteration_counts counts{ iterations };
                work_shop.spread_the_sweat( iterations, [&]( std::uint32_t const start, std::uint32_t const end ) noexcept { counts.ran( start, end ); } );
                wrong         .fetch_add( counts.not_run_exactly_once(), std::memory_order_relaxed );
                nested_spreads.fetch_add( 1                            , std::memory_order_relaxed );
            }
            spreading.fetch_sub( 1, std::memory_order_release );
        } );
    }

    for ( auto spread{ 0 }; spread < outside_spreads; ++spread )
    {
        iteration_counts counts{ iterations };
        work_shop.spread_the_sweat( iterations, [&]( std::uint32_t const start, std::uint32_t const end ) noexcept { counts.ran( start, end ); } );
        if ( counts.not_run_exactly_once() != 0 )
        {
            ADD_FAILURE() << "outside spread " << spread;
            break;
        }
    }
    EXPECT_TRUE( wait_for( [&] { return nested_spreads.load( std::memory_order_relaxed ) > 0; } ) );
    stop.store( true, std::memory_order_release );
    ASSERT_TRUE( wait_for( [&] { return spreading.load( std::memory_order_acquire ) == 0; } ) );
    EXPECT_EQ  ( wrong.load(), 0u );
    EXPECT_TRUE( settles( work_shop ) ) << work_shop.number_of_items() << " items left in the shop";
}

// A worker that keeps spreading while the rest of the pool stays busy does not
// pile work up for the others: once they are let go the shop empties.
TEST( NestedSpread, SpreadingOverAndOverOnABusyPoolLeavesNothingBehind )
{
    shop work_shop;
    if ( worker_threads( work_shop ) < 1 )
        GTEST_SKIP() << "needs a worker thread";

    auto const                 spreads   { std::uint32_t{ 100'000 } };
    auto const                 iterations{ std::uint32_t{ 64      } };
    std::atomic<std::uint64_t> ran{ 0 };
    ASSERT_TRUE( settles( work_shop ) );
    ASSERT_TRUE
    (
        with_every_worker_taken
        (
            work_shop,
            [&]() noexcept
            {
                for ( std::uint32_t spread{ 0 }; spread < spreads; ++spread )
                {
                    work_shop.spread_the_sweat( iterations, [&]( std::uint32_t const start, std::uint32_t const end ) noexcept
                    {
                        ran.fetch_add( end - start, std::memory_order_relaxed );
                    } );
                }
            },
            [] {}
        )
    );
    EXPECT_EQ  ( ran.load(), std::uint64_t{ spreads } * iterations );
    EXPECT_TRUE( settles( work_shop ) ) << work_shop.number_of_items() << " items left in the shop";

    // The pool is as good as new: a plain spread uses the workers again.
    if ( worker_threads( work_shop ) >= 2 )
    {
        thread_set threads;
        work_shop.spread_the_sweat( static_cast<std::uint32_t>( work_shop.number_of_workers() ) * 64, [&]( std::uint32_t, std::uint32_t ) noexcept
        {
            threads.add_current();
            wait_for( [&] { return threads.size() > 1; }, std::chrono::seconds{ 2 } );
        } );
        EXPECT_GT( threads.size(), 1u );
    }
}

#if PSI_SWEATER_USE_CALLER_THREAD
// A thread outside the pool that spreads while every worker is taken does the
// work itself instead of waiting for the workers.
TEST( NestedSpread, AnOutsideCallerDoesNotWaitForABusyPool )
{
    shop work_shop;
    if ( worker_threads( work_shop ) < 1 )
        GTEST_SKIP() << "needs a worker thread";

    auto const        iterations{ std::uint32_t{ 4096 } };
    iteration_counts  counts{ iterations };
    std::atomic<bool> finished        { false };
    std::atomic<bool> finished_in_time{ false };
    std::thread       caller;
    ASSERT_TRUE( settles( work_shop ) );
    auto const pool_was_taken
    {
        with_every_worker_taken
        (
            work_shop,
            []() noexcept {},
            [&]
            {
                caller = std::thread
                {
                    [&]
                    {
                        work_shop.spread_the_sweat( iterations, [&]( std::uint32_t const start, std::uint32_t const end ) noexcept
                        {
                            counts.ran( start, end );
                        } );
                        finished.store( true, std::memory_order_release );
                    }
                };
                finished_in_time.store( wait_for( [&] { return finished.load( std::memory_order_acquire ); }, std::chrono::seconds{ 5 } ), std::memory_order_release );
            }
        )
    };
    if ( caller.joinable() )
        caller.join(); // at the latest once the pool was let go
    ASSERT_TRUE( pool_was_taken );
    EXPECT_TRUE( finished_in_time.load() ) << "the spread waited for the workers to become free";
    EXPECT_EQ  ( counts.not_run_exactly_once(), 0u );
    EXPECT_TRUE( settles( work_shop ) ) << work_shop.number_of_items() << " items left in the shop";
}
#endif // PSI_SWEATER_USE_CALLER_THREAD

//------------------------------------------------------------------------------
} // namespace psi::sweater::generic
//------------------------------------------------------------------------------
