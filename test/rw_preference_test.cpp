//==============================================================================
// Behavior tests for the compile-time reader/writer-preference distinction
// (psi::thrd_lite::rw_preference, rw_mutex_traits, basic_rw_mutex<Preference>).
//
// This file only builds where reader_preferring_rw_mutex is defined (glibc NP
// rwlock-kind extensions -- see posix/rw_mutex.hpp); it is excluded from the
// Windows build by CMakeLists.txt, and the type simply does not exist on macOS
// (no NP extensions there either), so guard on the same macro the headers use.
//==============================================================================

#include <psi/sweater/threading/rrw_mutex.hpp>
#include <psi/sweater/threading/rw_mutex.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP

static_assert( rw_mutex_traits<rw_mutex>::preference == rw_preference::writer_preferring );
static_assert( rw_mutex_traits<rw_mutex>::supports_reader_preference );
static_assert( rw_mutex_traits<rw_mutex>::supports_writer_preference );

static_assert( rw_mutex_traits<reader_preferring_rw_mutex>::preference == rw_preference::reader_preferring );
static_assert( rw_mutex_traits<reader_preferring_rw_mutex>::supports_reader_preference );

// The whole point of reader_preferring_rw_mutex: a thread that already holds the
// read side can re-acquire it directly (no rrw_mutex wrapper, no per-thread hold
// tracking) even with a writer queued. Mirrors
// ReentrantRWMutex.NestedReadDoesNotDeadlockBehindQueuedWriter (rrw_mutex_test.cpp)
// but exercises the raw preference-selected mutex instead of the reentrant wrapper.
TEST( ReaderPreferringRWMutex, NestedReadDoesNotDeadlockBehindQueuedWriter )
{
    reader_preferring_rw_mutex m;
    std::atomic<bool> outer_held  { false };
    std::atomic<bool> writer_going{ false };
    std::atomic<bool> nested_done { false };

    std::thread reader{ [&]
    {
        reader_preferring_ro_lock outer{ m };
        outer_held = true;
        while ( !writer_going.load() )
        {
            std::this_thread::yield();
        }
        // let the writer actually reach (and queue behind the held read lock)
        std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } );
        reader_preferring_ro_lock nested{ m }; // would deadlock here under writer preference
        nested_done = true;
    } };

    while ( !outer_held.load() )
    {
        std::this_thread::yield();
    }

    std::thread writer{ [&]
    {
        writer_going = true;
        reader_preferring_rw_lock w{ m }; // must not block the nested reader above
    } };

    auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 5 } };
    while ( !nested_done.load() && ( std::chrono::steady_clock::now() < deadline ) )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds{ 5 } );
    }

    EXPECT_TRUE( nested_done.load() ) << "nested read acquire deadlocked behind a queued writer on a reader-preferring mutex";

    if ( !nested_done.load() )
    {
        // Genuine regression: leak the wedged threads rather than hang the test run.
        reader.detach();
        writer.detach();
        return;
    }
    reader.join();
    writer.join();
}

// Sanity: the two preferences are distinct types with independent state.
TEST( ReaderPreferringRWMutex, IndependentFromWriterPreferringMutex )
{
    rw_mutex                  w;
    reader_preferring_rw_mutex r;

    rw_lock                   wl{ w };
    reader_preferring_rw_lock rl{ r };

    EXPECT_TRUE( w.is_locked() );
    EXPECT_TRUE( r.is_locked() );
}

#else
TEST( ReaderPreferringRWMutex, NotAvailableOnThisPlatform )
{
    GTEST_SKIP() << "glibc NP rwlock-kind extensions unavailable -- reader_preferring_rw_mutex is not defined here";
}
#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
