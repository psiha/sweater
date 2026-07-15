//==============================================================================
// Behavior/correctness tests for the experimental futex-based rwlock
// (psi::thrd_lite::futex_rw_mutex, see futex_rw_mutex.hpp). Built directly on
// psi::thrd_lite::futex, so it links wherever that has a real backend: Linux
// (SYS_futex), Windows (WaitOnAddress), and Apple (__ulock_wait/__ulock_wake,
// private API -- see apple/futex.cpp).
//==============================================================================

#include <psi/sweater/threading/futex_rw_mutex.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <type_traits>
#include <vector>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// Regression test: futex's std::atomic base has a deleted copy ctor, which without
// futex_rw_mutex's own explicit copy/move ctors (mirroring rw_mutex's) would silently
// delete the copy/move ctors of ANY type embedding a futex_rw_mutex member (e.g.
// rama's BitmapIndex on Apple) -- not a compile error at this point, just a quietly
// non-copyable/non-movable enclosing type discovered far away at its own use site.
TEST( FutexRWMutex, IsCopyableAndMovable )
{
    static_assert( std::is_copy_constructible_v<futex_rw_mutex> );
    static_assert( std::is_move_constructible_v<futex_rw_mutex> );

    struct Owner { futex_rw_mutex mutex; };
    static_assert( std::is_copy_constructible_v<Owner> );
    static_assert( std::is_move_constructible_v<Owner> );

    Owner original;
    Owner copied{ original }; // must not deadlock/assert: source is dormant
    Owner moved{ std::move( original ) };
    EXPECT_FALSE( copied.mutex.is_locked() );
    EXPECT_FALSE( moved.mutex.is_locked() );
}

TEST( FutexRWMutex, UncontendedTryLock )
{
    futex_rw_mutex m;
    EXPECT_FALSE( m.is_locked() );

    EXPECT_TRUE( m.try_acquire_rw() );
    EXPECT_TRUE( m.is_locked() );
    EXPECT_FALSE( m.try_acquire_ro() ); // exclusive: no reader admitted while held
    EXPECT_FALSE( m.try_acquire_rw() ); // and no second writer either
    m.release_rw();
    EXPECT_FALSE( m.is_locked() );

    EXPECT_TRUE( m.try_acquire_ro() );
    // A second, concurrent reader -- from another thread: futex_rw_mutex is
    // non-recursive (same contract as rw_mutex), so a second same-thread
    // try_acquire_ro() would trip the debug read_recursion_registry tripwire
    // (it looks exactly like the documented same-thread recursive-read hang
    // hazard, even though here it would happen not to deadlock).
    std::thread reader2{ [&] { EXPECT_TRUE( m.try_acquire_ro() ); m.release_ro(); } };
    reader2.join();
    EXPECT_FALSE( m.try_acquire_rw() ); // writer excluded while any reader holds
    m.release_ro();
    EXPECT_FALSE( m.is_locked() );
}

// The defining property: once a writer is queued, a *new* reader (a different
// thread than the one already holding the read side) must not be admitted until
// the writer has run -- this is what "writer_preferring" means, mirrored from the
// pthread_rwlock / rw_mutex contract this prototype targets.
TEST( FutexRWMutex, WriterPreferenceBlocksNewReadersOnceQueued )
{
    futex_rw_mutex m;
    std::atomic<bool> reader1_held { false };
    std::atomic<bool> writer_queued{ false };
    std::atomic<bool> reader2_tried{ false };
    std::atomic<bool> reader2_admitted_before_writer{ false };
    std::atomic<bool> writer_done  { false };

    std::thread reader1{ [&]
    {
        m.acquire_ro();
        reader1_held = true;
        while ( !reader2_tried.load() )
        {
            std::this_thread::yield();
        }
        // give the writer time to actually queue and the (rejected) second reader
        // time to observe rejection, before releasing
        std::this_thread::sleep_for( std::chrono::milliseconds{ 100 } );
        m.release_ro();
    } };

    while ( !reader1_held.load() )
    {
        std::this_thread::yield();
    }

    std::thread writer{ [&]
    {
        // signal "about to queue" before the (blocking) acquire_rw call
        writer_queued = true;
        m.acquire_rw();
        writer_done = true;
        m.release_rw();
    } };

    while ( !writer_queued.load() )
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for( std::chrono::milliseconds{ 20 } ); // let the writer actually park

    std::thread reader2{ [&]
    {
        // Must NOT be admitted while the writer is queued (try, don't block, so
        // the test can observe the rejection deterministically).
        if ( m.try_acquire_ro() )
        {
            reader2_admitted_before_writer = true;
            m.release_ro();
        }
        reader2_tried = true;
    } };

    reader1.join();
    reader2.join();
    writer.join();

    EXPECT_FALSE( reader2_admitted_before_writer.load() )
        << "a new reader was admitted while a writer was queued -- not writer-preferring";
    EXPECT_TRUE( writer_done.load() );
    EXPECT_FALSE( m.is_locked() );
}

// Stress/invariant test: many reader and writer threads hammer a shared counter
// guarded by the mutex; an exclusive holder must observe itself as the sole
// holder (no concurrent reader or second writer), and readers must never observe
// a writer concurrently. Also exercises that every writer eventually gets in
// (no permanent starvation / no deadlock) under sustained contention.
TEST( FutexRWMutex, StressMutualExclusionAndProgress )
{
    futex_rw_mutex m;
    std::atomic<int>  active_readers{ 0 };
    std::atomic<bool> writer_active { false };
    std::atomic<bool> violation     { false };
    std::atomic<int>  writer_completions{ 0 };
    std::atomic<int>  reader_completions{ 0 };

    static constexpr int reader_threads = 6;
    static constexpr int writer_threads = 3;
    static constexpr int iterations     = 500;

    std::vector<std::thread> threads;
    threads.reserve( reader_threads + writer_threads );

    for ( int t{ 0 }; t < reader_threads; ++t )
    {
        threads.emplace_back( [&]
        {
            for ( int i{ 0 }; i < iterations; ++i )
            {
                m.acquire_ro();
                if ( writer_active.load( std::memory_order_acquire ) )
                {
                    violation = true;
                }
                active_readers.fetch_add( 1, std::memory_order_acq_rel );
                std::this_thread::yield();
                active_readers.fetch_sub( 1, std::memory_order_acq_rel );
                m.release_ro();
                ++reader_completions;
            }
        } );
    }
    for ( int t{ 0 }; t < writer_threads; ++t )
    {
        threads.emplace_back( [&]
        {
            for ( int i{ 0 }; i < iterations; ++i )
            {
                m.acquire_rw();
                if ( writer_active.exchange( true, std::memory_order_acq_rel ) )
                {
                    violation = true; // a second concurrent writer
                }
                if ( active_readers.load( std::memory_order_acquire ) != 0 )
                {
                    violation = true; // a writer concurrent with a reader
                }
                writer_active.store( false, std::memory_order_release );
                m.release_rw();
                ++writer_completions;
            }
        } );
    }

    for ( auto & th : threads )
    {
        th.join();
    }

    EXPECT_FALSE( violation.load() );
    EXPECT_EQ( reader_completions.load(), reader_threads * iterations );
    EXPECT_EQ( writer_completions.load(), writer_threads * iterations );
    EXPECT_FALSE( m.is_locked() );
}

//==============================================================================
// reader_preferring_futex_rw_mutex
//==============================================================================

TEST( ReaderPreferringFutexRWMutex, IsCopyableAndMovable )
{
    static_assert( std::is_copy_constructible_v<reader_preferring_futex_rw_mutex> );
    static_assert( std::is_move_constructible_v<reader_preferring_futex_rw_mutex> );
}

TEST( ReaderPreferringFutexRWMutex, UncontendedTryLock )
{
    reader_preferring_futex_rw_mutex m;
    EXPECT_FALSE( m.is_locked() );

    EXPECT_TRUE( m.try_acquire_rw() );
    EXPECT_FALSE( m.try_acquire_ro() ); // exclusive: no reader admitted while held
    EXPECT_FALSE( m.try_acquire_rw() );
    m.release_rw();
    EXPECT_FALSE( m.is_locked() );

    EXPECT_TRUE( m.try_acquire_ro() );
    EXPECT_FALSE( m.try_acquire_rw() ); // writer excluded while any reader holds
    m.release_ro();
    EXPECT_FALSE( m.is_locked() );
}

// The defining property, inverted from futex_rw_mutex's: a NEW reader (a different
// thread than the one already holding the read side) MUST be admitted even once a
// writer is queued -- that is what "reader preferring" means.
TEST( ReaderPreferringFutexRWMutex, NewReaderAdmittedDespiteQueuedWriter )
{
    reader_preferring_futex_rw_mutex m;
    std::atomic<bool> reader1_held      { false };
    std::atomic<bool> writer_queued     { false };
    std::atomic<bool> reader2_admitted  { false };
    std::atomic<bool> release_reader1   { false }; // test-driven: unblocks reader1 once the check below is done
    std::atomic<bool> writer_done       { false };

    std::thread reader1{ [&]
    {
        m.acquire_ro();
        reader1_held = true;
        while ( !release_reader1.load() )
        {
            std::this_thread::yield();
        }
        m.release_ro();
    } };

    while ( !reader1_held.load() )
    {
        std::this_thread::yield();
    }

    std::thread writer{ [&]
    {
        writer_queued = true;
        m.acquire_rw(); // blocks until reader1 releases (reader2 below is transient)
        writer_done = true;
        m.release_rw();
    } };

    while ( !writer_queued.load() )
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for( std::chrono::milliseconds{ 20 } ); // let the writer actually park

    std::thread reader2{ [&]
    {
        // Try, don't block: if reader preference isn't working this would hang
        // forever behind the queued writer instead of returning false quickly.
        if ( m.try_acquire_ro() )
        {
            reader2_admitted = true;
            m.release_ro();
        }
    } };
    reader2.join();

    EXPECT_TRUE( reader2_admitted.load() )
        << "a new reader was NOT admitted while a writer was queued -- not reader-preferring";
    EXPECT_FALSE( writer_done.load() ) << "writer should still be parked behind reader1";

    release_reader1 = true;
    reader1.join();
    writer.join();
    EXPECT_TRUE( writer_done.load() );
    EXPECT_FALSE( m.is_locked() );
}

// Same-thread nested reads must be natively deadlock-free here (no TLS hold-tracking
// needed, unlike rrw_mutex) even with a writer queued behind the OUTER hold. NOTE: the
// nested acquire_ro()/release_ro() pair below must not itself let the reader count
// reach zero (the outer hold is still live throughout), so the queued writer cannot
// and must not succeed until the outer hold is released too -- asserted explicitly
// below, since a test that instead looped "nested acquire/release until the writer
// succeeds" would live-lock forever (the outer hold never actually going away).
TEST( ReaderPreferringFutexRWMutex, NestedReadDoesNotDeadlockBehindQueuedWriter )
{
    reader_preferring_futex_rw_mutex m;
    std::atomic<bool> reader_held  { false };
    std::atomic<bool> writer_queued{ false };
    std::atomic<bool> writer_done  { false };

    std::thread writer{ [&]
    {
        while ( !reader_held.load() )
        {
            std::this_thread::yield();
        }
        writer_queued = true;
        m.acquire_rw(); // must block until the OUTER hold below releases
        writer_done = true;
        m.release_rw();
    } };

    m.acquire_ro();
    reader_held = true;
    while ( !writer_queued.load() )
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for( std::chrono::milliseconds{ 20 } ); // let the writer actually park

    // Nested acquire on the SAME thread while a writer is queued behind the outer
    // hold -- must return immediately rather than deadlock (this is the property
    // under test: if it deadlocked, this call itself would hang).
    m.acquire_ro();
    m.release_ro();
    EXPECT_FALSE( writer_done.load() ) << "writer should still be parked behind the outer hold";

    m.release_ro(); // release the outer hold -- only now can the writer succeed
    writer.join();
    EXPECT_TRUE( writer_done.load() );
    EXPECT_FALSE( m.is_locked() );
}

TEST( ReaderPreferringFutexRWMutex, StressMutualExclusionAndProgress )
{
    reader_preferring_futex_rw_mutex m;
    std::atomic<int>  active_readers{ 0 };
    std::atomic<bool> writer_active { false };
    std::atomic<bool> violation     { false };
    std::atomic<int>  writer_completions{ 0 };
    std::atomic<int>  reader_completions{ 0 };

    static constexpr int reader_threads = 6;
    static constexpr int writer_threads = 3;
    static constexpr int iterations     = 500;

    std::vector<std::thread> threads;
    threads.reserve( reader_threads + writer_threads );

    for ( int t{ 0 }; t < reader_threads; ++t )
    {
        threads.emplace_back( [&]
        {
            for ( int i{ 0 }; i < iterations; ++i )
            {
                m.acquire_ro();
                if ( writer_active.load( std::memory_order_acquire ) )
                {
                    violation = true;
                }
                active_readers.fetch_add( 1, std::memory_order_acq_rel );
                std::this_thread::yield();
                active_readers.fetch_sub( 1, std::memory_order_acq_rel );
                m.release_ro();
                ++reader_completions;
            }
        } );
    }
    for ( int t{ 0 }; t < writer_threads; ++t )
    {
        threads.emplace_back( [&]
        {
            for ( int i{ 0 }; i < iterations; ++i )
            {
                m.acquire_rw();
                if ( writer_active.exchange( true, std::memory_order_acq_rel ) )
                {
                    violation = true; // a second concurrent writer
                }
                if ( active_readers.load( std::memory_order_acquire ) != 0 )
                {
                    violation = true; // a writer concurrent with a reader
                }
                writer_active.store( false, std::memory_order_release );
                m.release_rw();
                ++writer_completions;
            }
        } );
    }

    for ( auto & th : threads )
    {
        th.join();
    }

    EXPECT_FALSE( violation.load() );
    EXPECT_EQ( reader_completions.load(), reader_threads * iterations );
    EXPECT_EQ( writer_completions.load(), writer_threads * iterations );
    EXPECT_FALSE( m.is_locked() );
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
