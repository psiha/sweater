//==============================================================================
// Behavior tests for reader_preferring_rw_mutex / rrw_mutex on platforms where
// reader preference is a genuine OS-level primitive (glibc NP rwlock-kind
// extensions -- see posix/rw_mutex.hpp): reader_preferring_rw_mutex there is a
// thin rw_mutex subclass, not the per-thread hold-tracking wrapper
// (basic_rrw_mutex, rrw_mutex.hpp) that backs the same names on every other
// platform (that mechanism already has its own coverage in rrw_mutex_test.cpp,
// and this file's first test intentionally mirrors it as a cross-check).
//
// This file only builds where reader_preferring_rw_mutex is defined this way
// (glibc NP rwlock-kind extensions); it is excluded from the Windows build by
// CMakeLists.txt, and the type simply does not exist that way on macOS (no NP
// extensions there either), so guard on the same macro the headers use.
//==============================================================================

#include <psi/sweater/threading/rrw_mutex.hpp>
#include <psi/sweater/threading/rw_mutex.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <type_traits>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP

// writer_preferring_rw_mutex is rw_mutex itself; reader_preferring_rw_mutex (and so
// rrw_mutex) is a distinct type that derives from it -- both bind to a plain
// rw_mutex& (rw_lock's parameter type), so no separate write-guard type is needed.
static_assert(  std::is_same_v<writer_preferring_rw_mutex, rw_mutex> );
static_assert( !std::is_same_v<reader_preferring_rw_mutex, rw_mutex> );
static_assert(  std::is_base_of_v<rw_mutex, reader_preferring_rw_mutex> );
static_assert(  std::is_same_v<rrw_mutex, reader_preferring_rw_mutex> );

// The whole point of reader_preferring_rw_mutex: a thread that already holds the
// read side can re-acquire it directly (no per-thread hold-tracking machinery, no
// separate mechanism from what rrw_mutex names on this platform) even with a
// writer queued. Mirrors ReentrantRWMutex.NestedReadDoesNotDeadlockBehindQueuedWriter
// (rrw_mutex_test.cpp) -- same observable contract, different (true OS reader-
// preference, not TLS hold-collapsing) mechanism underneath on this platform.
TEST( ReaderPreferringRWMutex, NestedReadDoesNotDeadlockBehindQueuedWriter )
{
    reader_preferring_rw_mutex m;
    std::atomic<bool> outer_held  { false };
    std::atomic<bool> writer_going{ false };
    std::atomic<bool> nested_done { false };

    std::thread reader{ [&]
    {
        rro_lock outer{ m };
        outer_held = true;
        while ( !writer_going.load() )
        {
            std::this_thread::yield();
        }
        // let the writer actually reach (and queue behind the held read lock)
        std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } );
        rro_lock nested{ m }; // would deadlock here under writer preference
        nested_done = true;
    } };

    while ( !outer_held.load() )
    {
        std::this_thread::yield();
    }

    std::thread writer{ [&]
    {
        writer_going = true;
        rw_lock w{ m }; // must not block the nested reader above
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

// Sanity: the two preferences are distinct types with independent state, and the
// SAME rw_lock/ro_lock-family guards work for both (no preference-specific guard
// types -- that's the point of the redesign this test pins).
TEST( ReaderPreferringRWMutex, IndependentFromWriterPreferringMutex )
{
    rw_mutex                   w;
    reader_preferring_rw_mutex r;

    rw_lock w_lock{ w };
    rw_lock r_lock{ r }; // same guard type as above -- accepts r by upcast to rw_mutex&

    EXPECT_TRUE( w.is_locked() );
    EXPECT_TRUE( r.is_locked() );
}

#else
TEST( ReaderPreferringRWMutex, NotAvailableOnThisPlatform )
{
    GTEST_SKIP() << "glibc NP rwlock-kind extensions unavailable -- reader_preferring_rw_mutex "
                    "here is rrw_mutex's per-thread hold-tracking wrapper, already covered by "
                    "rrw_mutex_test.cpp, not a distinct mechanism worth a separate test";
}
#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
