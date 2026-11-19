#include <psi/sweater/sweater.hpp>

#include "affinity_relaunch.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#ifdef __linux__
#   include <pthread.h>
#   include <sys/resource.h>
#endif

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
    auto future{ work_shop.dispatch( []() noexcept -> int { return 42; } ) }; // non-const: std::future::get() (native impls) is mutable
    EXPECT_EQ( future.get(), 42 );
}

TEST( SweaterSmoke, DispatchLiteReturnsValue )
{
    psi::sweater::shop work_shop;
    auto future{ work_shop.dispatch_lite( []() noexcept -> int { return 42; } ) };
    EXPECT_EQ( future.get(), 42 );
}

TEST( SweaterSmoke, DispatchLiteVoidResult )
{
    psi::sweater::shop work_shop;
    auto future{ work_shop.dispatch_lite( []() noexcept {} ) };
    future.get(); // must not throw
}

#if PSI_SWEATER_HAS_OUTCOME
TEST( SweaterSmoke, DispatchOutcomeReturnsValue )
{
    psi::sweater::shop work_shop;
    auto future{ work_shop.dispatch_outcome( []() noexcept -> int { return 42; } ) };
    auto const result( future.get() ); // noexcept -- never throws
    ASSERT_TRUE( result.has_value() );
    EXPECT_EQ( result.value(), 42 );
}

TEST( SweaterSmoke, DispatchOutcomeCapturesException )
{
    psi::sweater::shop work_shop;
    auto future{ work_shop.dispatch_outcome( []() -> int { throw std::runtime_error{ "boom" }; } ) };
    auto const result( future.get() );
    ASSERT_TRUE( result.has_exception() );
}
#endif // PSI_SWEATER_HAS_OUTCOME

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

//------------------------------------------------------------------------------
// The cases below hold for any parallelism. Their point is a parallelism of
// one (a single CPU machine, affinity mask or container quota): the shop then
// has no pool threads, a spread runs on its caller and each fired item on a
// detached thread of its own, which nothing joins - the tests that fire wait
// for their fired work with wait_until_idle() before their locals go away.
// EveryCaseWithOneCpu runs this whole file that way.
//------------------------------------------------------------------------------

namespace
{
    template <typename Shop, typename Work>
    void fire( Shop & shop, Work && work ) noexcept
    {
        if constexpr ( std::is_void_v<decltype( shop.fire_and_forget( std::forward<Work>( work ) ) )> )
            shop.fire_and_forget( std::forward<Work>( work ) );
        else
            (void)shop.fire_and_forget( std::forward<Work>( work ) );
    }

    // Releases the work waiting for `flag` and waits for all fired work to
    // end, also on a return from a failed assertion.
    struct sets_on_exit
    {
        std::atomic<bool> & flag;
        ~sets_on_exit() noexcept
        {
            flag.store( true, std::memory_order_release );
            (void)psi::sweater::wait_until_idle( std::chrono::seconds( 30 ) );
        }
    };

    bool wait_for( std::atomic<bool> const & flag ) noexcept
    {
        auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds( 30 ) };
        while ( !flag.load( std::memory_order_acquire ) && std::chrono::steady_clock::now() < deadline )
        {
            std::this_thread::yield();
        }
        return flag.load( std::memory_order_acquire );
    }
} // anonymous namespace

// Fired work is asynchronous: here it cannot finish before the fire call has
// returned, so a shop that ran it inside the call would never return.
TEST( SweaterSmoke, FiredWorkDoesNotRunInsideTheFireCall )
{
    std::atomic<bool> fire_returned{ false };
    std::atomic<bool> done         { false };
    psi::sweater::shop work_shop;
    fire( work_shop, [&]() noexcept
    {
        while ( !fire_returned.load( std::memory_order_acquire ) )
        {
            std::this_thread::yield();
        }
        done.store( true, std::memory_order_release );
    } );
    fire_returned.store( true, std::memory_order_release );
    EXPECT_TRUE( wait_for( done ) );
    EXPECT_TRUE( psi::sweater::wait_until_idle( std::chrono::seconds( 30 ) ) );
}

TEST( SweaterSmoke, SpreadCompletesWhileFiredWorkIsInFlight )
{
    std::atomic<bool> started{ false };
    std::atomic<bool> release{ false };
    std::atomic<bool> done   { false };
    std::atomic<int>  sum    { 0 };
    psi::sweater::shop work_shop;
    sets_on_exit const releases{ release };
    fire( work_shop, [&]() noexcept
    {
        started.store( true, std::memory_order_release );
        while ( !release.load( std::memory_order_acquire ) )
        {
            std::this_thread::yield();
        }
        done.store( true, std::memory_order_release );
    } );
    ASSERT_TRUE( wait_for( started ) );
    work_shop.spread_the_sweat( 1000, [&]( auto const start, auto const end ) noexcept
    {
        sum.fetch_add( static_cast<int>( end - start ), std::memory_order_relaxed );
    } );
    EXPECT_EQ( sum.load(), 1000 );
    release.store( true, std::memory_order_release );
    EXPECT_TRUE( wait_for( done ) );
}

TEST( SweaterSmoke, FiredWorkCanSpreadAndFire )
{
    std::atomic<bool> done{ false };
    std::atomic<int>  sum { 0 };
    psi::sweater::shop work_shop;
    fire( work_shop, [&]() noexcept
    {
        work_shop.spread_the_sweat( 1000, [&]( auto const start, auto const end ) noexcept
        {
            sum.fetch_add( static_cast<int>( end - start ), std::memory_order_relaxed );
        } );
        fire( work_shop, [&]() noexcept { done.store( true, std::memory_order_release ); } );
    } );
    EXPECT_TRUE( wait_for( done ) );
    EXPECT_TRUE( psi::sweater::wait_until_idle( std::chrono::seconds( 30 ) ) );
    EXPECT_EQ( sum.load(), 1000 );
}

#ifdef __linux__ // the pool-of-threads implementation
// With a parallelism of one a spread is plain serial code on its caller: no
// pool thread takes part in it, whether or not the shop holds fired work.
TEST( SweaterSmoke, AParallelismOfOneSpreadsOnTheCaller )
{
    psi::sweater::shop work_shop;
    if ( psi::sweater::test::restricted_cpus() == 1 )
    {
        ASSERT_EQ( static_cast<int>( psi::thrd_lite::hardware_concurrency_max ), 1 );
    }
    else
    {
        work_shop.set_max_allowed_threads( 1 );
    }
    EXPECT_EQ( work_shop.number_of_workers(), 1 );

    auto const caller{ std::this_thread::get_id() };
    std::atomic<int> sum        { 0 };
    std::atomic<int> off_caller { 0 };
    auto const spread
    {
        [&]
        {
            work_shop.spread_the_sweat( 1000, [&]( auto const start, auto const end ) noexcept
            {
                if ( std::this_thread::get_id() != caller )
                    off_caller.fetch_add( 1, std::memory_order_relaxed );
                sum.fetch_add( static_cast<int>( end - start ), std::memory_order_relaxed );
            } );
        }
    };
    spread();

    std::atomic<bool> started{ false };
    std::atomic<bool> release{ false };
    std::atomic<bool> done   { false };
    sets_on_exit const releases{ release };
    fire( work_shop, [&]() noexcept
    {
        started.store( true, std::memory_order_release );
        while ( !release.load( std::memory_order_acquire ) )
        {
            std::this_thread::yield();
        }
        done.store( true, std::memory_order_release );
    } );
    ASSERT_TRUE( wait_for( started ) );
    spread();
    release.store( true, std::memory_order_release );
    EXPECT_TRUE( wait_for( done ) );

    EXPECT_EQ( sum.load(), 2000 );
    EXPECT_EQ( off_caller.load(), 0 );

    auto future{ work_shop.dispatch( []() noexcept -> int { return 42; } ) };
    EXPECT_EQ( future.get(), 42 );

    // There is no pool thread to bind.
    EXPECT_FALSE( work_shop.bind_worker_to_cpu( 0, static_cast<unsigned>( ::sched_getcpu() ) ) );
    EXPECT_TRUE( psi::sweater::wait_until_idle( std::chrono::seconds( 30 ) ) );
}

// A thread that cannot be created is a failed fire: reported as one, the work
// neither run nor leaked, and the shop usable afterwards.
TEST( SweaterSmoke, AParallelismOfOneReportsAThreadItCannotCreate )
{
    ::rlimit original;
    ASSERT_EQ( ::getrlimit( RLIMIT_NPROC, &original ), 0 );
    struct restores_limit
    {
        ::rlimit const & limit;
        ~restores_limit() noexcept { ::setrlimit( RLIMIT_NPROC, &limit ); }
    };

    struct counted
    {
        counted( std::atomic<int> & alive ) noexcept : alive{ &alive } { alive.fetch_add( 1 ); }
        counted( counted && other ) noexcept : alive{ std::exchange( other.alive, nullptr ) } {}
        ~counted() noexcept { if ( alive ) alive->fetch_sub( 1 ); }
        std::atomic<int> * alive;
    };
    std::atomic<int> captures_alive{ 0 };
    std::atomic<int> runs          { 0 };

    psi::sweater::shop work_shop;
    work_shop.set_max_allowed_threads( 1 );
    {
        restores_limit const restore{ original };
        // No more threads for this user (the limit counts those of all its
        // processes; it does not apply to a privileged process).
        ::rlimit const none{ .rlim_cur = 1, .rlim_max = original.rlim_max };
        ASSERT_EQ( ::setrlimit( RLIMIT_NPROC, &none ), 0 );
        {
            pthread_t probe;
            if ( ::pthread_create( &probe, nullptr, []( void * ) -> void * { return nullptr; }, nullptr ) == 0 )
            {
                ::pthread_join( probe, nullptr );
                GTEST_SKIP() << "the thread limit does not apply to this process";
            }
        }

        EXPECT_FALSE( work_shop.fire_and_forget( [ &runs, capture = counted{ captures_alive } ]() noexcept { runs.fetch_add( 1 ); } ) );
        EXPECT_EQ( captures_alive.load(), 0 );

        auto future{ work_shop.dispatch( [ &runs, capture = counted{ captures_alive } ]() noexcept -> int { runs.fetch_add( 1 ); return 1; } ) };
        EXPECT_THROW( future.get(), std::bad_alloc );
        auto lite_future{ work_shop.dispatch_lite( [ &runs, capture = counted{ captures_alive } ]() noexcept -> int { runs.fetch_add( 1 ); return 1; } ) };
        EXPECT_THROW( lite_future.get(), std::bad_alloc );
        EXPECT_EQ( captures_alive.load(), 0 );
    }
    EXPECT_EQ( runs.load(), 0 );
    EXPECT_TRUE( psi::sweater::wait_until_idle( std::chrono::seconds( 30 ) ) );

    auto after{ work_shop.dispatch( []() noexcept -> int { return 43; } ) };
    EXPECT_EQ( after.get(), 43 );
}
#endif // __linux__

#if defined( __linux__ ) || defined( _WIN32 )
TEST( SweaterSmoke, EveryCaseWithOneCpu )
{
    if ( psi::sweater::test::restricted_cpus() )
        GTEST_SKIP() << "this is the restricted child";
    if ( psi::sweater::test::available_cpu_count() < 1 )
        GTEST_SKIP() << "the affinity mask of the process cannot be read";
    EXPECT_TRUE( psi::sweater::test::run_under_affinity( 1, "SweaterSmoke.*-SweaterSmoke.EveryCaseWithOneCpu" ) );
}
#endif
