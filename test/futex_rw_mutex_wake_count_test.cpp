//==============================================================================
// Futex-traffic tests for the futex_rw_mutex family: a wake must be paid per
// writer sleep announcement, never per reader release.
//
// This translation unit is built together with futex_rw_mutex.cpp compiled with
// PSI_THRD_LITE_FUTEX_RW_MUTEX_COUNTERS (see test/CMakeLists.txt), which makes the
// lock count its own wait/wake calls in futex_rw_mutex_call_counters. The
// library proper is never built with that macro.
//==============================================================================

#ifndef PSI_THRD_LITE_FUTEX_RW_MUTEX_COUNTERS
#error "build with PSI_THRD_LITE_FUTEX_RW_MUTEX_COUNTERS (and compile futex_rw_mutex.cpp with it too)"
#endif

#include "mutex_test_traits.hpp"

#include <psi/sweater/threading/futex_rw_mutex.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

namespace
{
    struct TypeNames
    {
        template <class T>
        static std::string GetName( int )
        {
            if constexpr ( std::is_same_v<T, futex_rw_mutex> ) { return "futex_rw_mutex"; }
            else                                               { return "reader_preferring_futex_rw_mutex"; }
        }
    };

    auto & counters() noexcept { return futex_rw_mutex_call_counters; }

    void reset_counters() noexcept
    {
        counters().writer_announces.store( 0 );
        counters().writer_waits    .store( 0 );
        counters().release_ro_wakes.store( 0 );
        counters().release_rw_wakes.store( 0 );
    }

    auto load( std::atomic<std::uint64_t> const & counter ) noexcept { return counter.load(); }

    // Spin until the writer has reached the futex (it counts the call just before
    // issuing it), then give the kernel a moment to actually put it to sleep.
    void wait_for_writer_to_park()
    {
        while ( load( counters().writer_waits ) == 0 )
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } );
    }
} // anonymous namespace

template <class Mutex>
struct FutexWakeCount : ::testing::Test
{
    void SetUp() override { reset_counters(); }
};

using FutexWakeCountTypes = ::testing::Types<futex_rw_mutex, reader_preferring_futex_rw_mutex>;
TYPED_TEST_SUITE( FutexWakeCount, FutexWakeCountTypes, TypeNames );

// Nobody sleeps => no futex traffic at all, not even a wake per release.
TYPED_TEST( FutexWakeCount, UncontendedUseMakesNoFutexCalls )
{
    TypeParam m;
    for ( int i{ 0 }; i < 1000; ++i )
    {
        m.acquire_ro(); m.release_ro();
        m.acquire_rw(); m.release_rw();
    }
    EXPECT_EQ( load( counters().writer_waits     ), 0u );
    EXPECT_EQ( load( counters().release_ro_wakes ), 0u );
    EXPECT_EQ( load( counters().release_rw_wakes ), 0u );
}

// A writer asleep behind a reader costs exactly one wake, issued by the release that
// drops the last read hold.
TYPED_TEST( FutexWakeCount, WriterSleepingBehindAReaderIsWokenOnce )
{
    TypeParam m;
    m.acquire_ro();
    std::thread writer{ [&] { m.acquire_rw(); m.release_rw(); } };
    wait_for_writer_to_park();
    EXPECT_EQ( load( counters().release_ro_wakes ), 0u );

    m.release_ro();
    writer.join();
    EXPECT_EQ( load( counters().release_ro_wakes ), 1u );
    EXPECT_EQ( load( counters().writer_announces ), 1u );
    EXPECT_FALSE( m.is_locked() );
}

// Reader hand-over that never lets the count reach zero, while a writer sleeps: no
// wake may be issued however long the churn lasts, and the final drain wakes once.
// Reader-preferring only: the writer-preferring lock admits no new reader once the
// writer is queued.
TYPED_TEST( FutexWakeCount, ReaderChurnWithoutZeroCrossingMakesNoWake )
{
    if constexpr ( !test::mutex_traits<TypeParam>::admits_reader_with_writer_queued )
    {
        GTEST_SKIP() << "new readers are not admitted behind a queued writer";
    }
    else
    {
        TypeParam m;
        m.acquire_ro();
        std::thread writer{ [&] { m.acquire_rw(); m.release_rw(); } };
        wait_for_writer_to_park();

        for ( int i{ 0 }; i < 20000; ++i )
        {
            m.acquire_ro(); // count 1 -> 2 -> 1: never zero
            m.release_ro();
        }
        EXPECT_EQ( load( counters().release_ro_wakes ), 0u );

        m.release_ro();
        writer.join();
        EXPECT_EQ( load( counters().release_ro_wakes ), 1u );
    }
}

// Churn with the count passing through zero all the time: the wakes issued by reader
// releases never exceed the writer's sleep announcements, however many releases
// there were -- a wake per zero crossing would exceed them by orders of magnitude.
TYPED_TEST( FutexWakeCount, ReleaseWakesAreBoundedByWriterAnnouncementsNotReaderChurn )
{
    TypeParam m;
    std::atomic<bool>          stop    { false };
    std::atomic<std::uint64_t> releases{ 0 };

    static constexpr int reader_threads = 6;
    static constexpr int writes_wanted  = 100;

    std::vector<std::thread> readers;
    readers.reserve( reader_threads );
    for ( int t{ 0 }; t < reader_threads; ++t )
    {
        readers.emplace_back( [&]
        {
            while ( !stop.load( std::memory_order_relaxed ) )
            {
                m.acquire_ro();
                m.release_ro();
                releases.fetch_add( 1, std::memory_order_relaxed );
                std::this_thread::yield();
            }
        } );
    }

    // The churn must be underway before the writer starts and must go on for a
    // minimum volume of reader releases, whatever the writer's pace.
    static constexpr std::uint64_t min_releases = 200'000;
    while ( releases.load( std::memory_order_relaxed ) < 1000 )
    {
        std::this_thread::yield();
    }
    auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 60 } };
    int writes{ 0 };
    while
    (
        ( writes < writes_wanted || releases.load( std::memory_order_relaxed ) < min_releases ) &&
        std::chrono::steady_clock::now() < deadline
    )
    {
        m.acquire_rw();
        m.release_rw();
        ++writes;
    }
    stop = true;
    for ( auto & reader : readers )
    {
        reader.join();
    }

    EXPECT_GE( writes, writes_wanted ) << "the writer starved behind the reader churn";
    EXPECT_GE( releases.load(), min_releases );
    auto const announces{ load( counters().writer_announces ) };
    auto const ro_wakes { load( counters().release_ro_wakes ) };
    EXPECT_LE( ro_wakes, announces );
    std::printf
    (
        "writer acquisitions %d, reader releases %llu, writer announces %llu, writer waits %llu, release_ro wakes %llu, release_rw wakes %llu\n",
        writes, static_cast<unsigned long long>( releases.load() ), static_cast<unsigned long long>( announces ),
        static_cast<unsigned long long>( load( counters().writer_waits ) ), static_cast<unsigned long long>( ro_wakes ),
        static_cast<unsigned long long>( load( counters().release_rw_wakes ) )
    );
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
