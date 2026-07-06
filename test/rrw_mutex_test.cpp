//==============================================================================
// Behavior tests for the reentrant-read mutex (psi::thrd_lite::rrw_mutex).
//
// The underlying OS primitive (pthread_rwlock_t / SRWLOCK) is writer-preferring and
// NON-recursive: a thread that already holds the read side and then asks for it again
// while a writer is queued deadlocks. rrw_mutex makes only the outermost read acquire
// (per thread) touch the OS lock, so a nested read acquire cannot block behind a queued
// writer. These tests pin that contract; the write side stays non-recursive and mutually
// exclusive with reads.
//==============================================================================

#include <psi/sweater/threading/rrw_mutex.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// Nested read acquisition on one thread is allowed (no self-deadlock, no assert), and
// once every read hold is released the write side becomes available again.
TEST( ReentrantRWMutex, NestedReadOnSameThreadThenWriteAfterRelease )
{
    rrw_mutex m;
    {
        rro_lock outer{ m };
        {
            rro_lock nested{ m };               // reentrant: must not hang / assert
            EXPECT_TRUE( m.try_acquire_ro() );  // a reentrant try while held also succeeds
            m.release_ro();
        }
        // outer still held here -> the write side must be unavailable
        EXPECT_FALSE( m.try_acquire_rw() );
    }
    // all read holds released -> the OS read lock was dropped, write side free again
    EXPECT_TRUE( m.try_acquire_rw() );
    m.release_rw();
}

// The regression: a nested read acquire on a thread already holding the read lock must
// NOT block behind a writer that is queued for the exclusive lock. On a non-recursive
// rwlock this is the documented deadlock; the reentrant mutex turns the nested acquire
// into a depth bump that never touches the OS lock.
TEST( ReentrantRWMutex, NestedReadDoesNotDeadlockBehindQueuedWriter )
{
    rrw_mutex m;
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
        // let the writer actually reach (and block inside) acquire_rw
        std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } );
        rro_lock nested{ m };               // would deadlock here without reentrancy
        nested_done = true;
        // scope end: release nested (depth bump only) then outer (real OS unlock)
    } };

    while ( !outer_held.load() )
    {
        std::this_thread::yield();
    }

    std::thread writer{ [&]
    {
        writer_going = true;
        rw_lock w{ m };                     // blocks until the reader fully releases
    } };

    auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 5 } };
    while ( !nested_done.load() && ( std::chrono::steady_clock::now() < deadline ) )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds{ 5 } );
    }

    EXPECT_TRUE( nested_done.load() ) << "nested read acquire deadlocked behind a queued writer";

    if ( !nested_done.load() )
    {
        // A genuine regression: the reader is wedged in the nested acquire and the writer
        // behind it. Don't join (that would hang the test run); leak the wedged threads --
        // the failure is already recorded and the process is on its way down.
        reader.detach();
        writer.detach();
        return;
    }
    reader.join();
    writer.join();
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
