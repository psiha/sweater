//==============================================================================
// Parameterized (TYPED_TEST_SUITE) consolidation of the rw-mutex family's
// shared behavioral contracts. Supersedes the former rw_mutex_test.cpp,
// rrw_mutex_test.cpp and futex_rw_mutex_test.cpp, which hand-duplicated the
// same properties once per concrete type -- see README.md's Testing section
// for the history. rw_preference_test.cpp remains separate: it only carries
// type-identity static_asserts that only make sense standing alone.
//
// Suite map (mirrors the coverage matrix the superseded files actually had --
// consolidation does not add new platform coverage, only removes duplication):
//   - MutexContract           : rw_mutex, futex_rw_mutex,
//                               reader_preferring_rw_mutex,
//                               reader_preferring_futex_rw_mutex
//       Preference-independent mutual-exclusion invariants.
//   - WriterPreferringContract: rw_mutex, futex_rw_mutex
//       A new reader is blocked once a writer is queued.
//   - ReaderPreferringContract: reader_preferring_rw_mutex,
//                               reader_preferring_futex_rw_mutex
//       Same-thread nested reads never deadlock behind a queued writer
//       (unconditional on every platform -- both mechanisms guarantee this,
//       see rrw_mutex.hpp and futex_rw_mutex.hpp). New-reader-admission is
//       trait-gated (mutex_test_traits.hpp): unconditional for the futex
//       type, glibc-only for reader_preferring_rw_mutex (see rrw_mutex.hpp:
//       everywhere else that name is basic_rrw_mutex, which is "still
//       writer-preferring underneath").
//   - GuardContract           : rw_mutex, reader_preferring_rw_mutex
//       RAII guards / std::shared_lock interop -- these two are the only
//       types rw_lock/basic_ro_lock bind to (futex_rw_mutex and
//       reader_preferring_futex_rw_mutex are a separate, non-rw_mutex-
//       derived hierarchy, see futex_rw_mutex.hpp).
//   - FutexOnlyContract       : futex_rw_mutex, reader_preferring_futex_rw_mutex
//       Properties the two futex_rw_mutex.hpp types already shared with each
//       other (copyability, stress) but not with the rw_mutex family --
//       deliberately NOT folded into MutexContract: neither
//       reader_preferring_rw_mutex nor plain rw_mutex had stress coverage
//       before this consolidation, and a true reader-preferring lock (the
//       glibc reader_preferring_rw_mutex) can legitimately starve a writer
//       under this stress shape's sustained read load, which would hang the
//       test rather than fail it cleanly. Adding that coverage is a separate,
//       deliberate decision, not a byproduct of de-duplication.
//==============================================================================

#include "mutex_test_traits.hpp"

#include <psi/sweater/threading/futex_rw_mutex.hpp>
#include <psi/sweater/threading/rrw_mutex.hpp>
#include <psi/sweater/threading/rw_mutex.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
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
            if constexpr ( std::is_same_v<T, rw_mutex> ) { return "rw_mutex"; }
            else
            if constexpr ( std::is_same_v<T, futex_rw_mutex> ) { return "futex_rw_mutex"; }
            else
            if constexpr ( std::is_same_v<T, reader_preferring_rw_mutex> ) { return "reader_preferring_rw_mutex"; }
            else
            if constexpr ( std::is_same_v<T, reader_preferring_futex_rw_mutex> ) { return "reader_preferring_futex_rw_mutex"; }
            else
            { return "unknown_mutex_type"; }
        }
    };
} // anonymous namespace

//==============================================================================
// MutexContract
//==============================================================================

template <class Mutex>
struct MutexContract : ::testing::Test {};

using MutexContractTypes = ::testing::Types
<
    rw_mutex,
    futex_rw_mutex,
    reader_preferring_rw_mutex,
    reader_preferring_futex_rw_mutex
>;
TYPED_TEST_SUITE( MutexContract, MutexContractTypes, TypeNames );

TYPED_TEST( MutexContract, UncontendedTryLock )
{
    TypeParam m;
    EXPECT_FALSE( m.is_locked() );

    EXPECT_TRUE( m.try_acquire_rw() );
    EXPECT_TRUE( m.is_locked() );
    EXPECT_FALSE( m.try_acquire_ro() ); // exclusive: no reader admitted while held
    EXPECT_FALSE( m.try_acquire_rw() ); // and no second writer either
    m.release_rw();
    EXPECT_FALSE( m.is_locked() );

    EXPECT_TRUE( m.try_acquire_ro() );
    EXPECT_TRUE( m.is_locked() );
    EXPECT_FALSE( m.try_acquire_rw() ); // writer excluded while any reader holds
    m.release_ro();
    EXPECT_FALSE( m.is_locked() );
}

TYPED_TEST( MutexContract, SharedReadersConcurrentExcludeWriter )
{
    TypeParam m;
    m.acquire_ro();

    std::atomic<bool> other_read_ok { false };
    std::atomic<bool> write_excluded{ false };
    std::thread other{ [&]
    {
        if ( m.try_acquire_ro() ) // a second thread can read concurrently
        {
            other_read_ok = true;
            write_excluded = !m.try_acquire_rw(); // ...but nobody can write
            m.release_ro();
        }
    } };
    other.join();
    EXPECT_TRUE( other_read_ok .load() );
    EXPECT_TRUE( write_excluded.load() );

    m.release_ro();
    EXPECT_TRUE( m.try_acquire_rw() );
    m.release_rw();
}

TYPED_TEST( MutexContract, WriterExcludesEverything )
{
    TypeParam m;
    m.acquire_rw();

    std::atomic<int> failures{ 0 };
    std::thread other{ [&]
    {
        if ( !m.try_acquire_ro() ) { ++failures; } else { m.release_ro(); }
        if ( !m.try_acquire_rw() ) { ++failures; } else { m.release_rw(); }
    } };
    other.join();
    EXPECT_EQ( failures.load(), 2 );

    m.release_rw();
    EXPECT_TRUE( m.try_acquire_rw() );
    m.release_rw();
}

TYPED_TEST( MutexContract, WriterBlocksUntilReaderReleases )
{
    TypeParam m;
    std::atomic<bool> writer_acquired{ false };

    m.acquire_ro();
    std::thread writer{ [&]
    {
        m.acquire_rw();
        writer_acquired = true;
        m.release_rw();
    } };

    std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } );
    EXPECT_FALSE( writer_acquired.load() ); // still parked behind the read hold
    m.release_ro();
    writer.join();
    EXPECT_TRUE( writer_acquired.load() );
}

//==============================================================================
// WriterPreferringContract
//==============================================================================

template <class Mutex>
struct WriterPreferringContract : ::testing::Test {};

using WriterPreferringTypes = ::testing::Types<rw_mutex, futex_rw_mutex>;
TYPED_TEST_SUITE( WriterPreferringContract, WriterPreferringTypes, TypeNames );

// The defining property: once a writer is queued, a *new* reader (a different
// thread than the one already holding the read side) must not be admitted until
// the writer has run.
TYPED_TEST( WriterPreferringContract, NewReaderBlockedOnceWriterQueued )
{
    TypeParam m;
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
        std::this_thread::sleep_for( std::chrono::milliseconds{ 100 } );
        m.release_ro();
    } };

    while ( !reader1_held.load() )
    {
        std::this_thread::yield();
    }

    std::thread writer{ [&]
    {
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

//==============================================================================
// ReaderPreferringContract
//==============================================================================

template <class Mutex>
struct ReaderPreferringContract : ::testing::Test {};

using ReaderPreferringTypes = ::testing::Types<reader_preferring_rw_mutex, reader_preferring_futex_rw_mutex>;
TYPED_TEST_SUITE( ReaderPreferringContract, ReaderPreferringTypes, TypeNames );

// Uncontended sanity: a nested same-thread read succeeds without hanging/asserting,
// and the write side is unavailable until every read hold (nested + outer) is
// released.
TYPED_TEST( ReaderPreferringContract, NestedReadUncontended )
{
    TypeParam m;
    m.acquire_ro();
    EXPECT_TRUE( m.try_acquire_ro() ); // reentrant: same thread, already held
    m.release_ro();
    EXPECT_FALSE( m.try_acquire_rw() ); // outer hold still live
    m.release_ro();
    EXPECT_TRUE( m.try_acquire_rw() );
    m.release_rw();
}

// The defining property under contention: a nested read acquire on a thread
// already holding the read lock must not block behind a writer that is queued
// for the exclusive lock. Unconditional on every platform for both types here --
// see this file's header comment.
TYPED_TEST( ReaderPreferringContract, NestedReadDoesNotDeadlockBehindQueuedWriter )
{
    TypeParam m;
    std::atomic<bool> reader_held  { false };
    std::atomic<bool> writer_queued{ false };
    std::atomic<bool> nested_done  { false };
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
    std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } ); // let the writer actually park

    m.acquire_ro(); // would deadlock here without reader-preference/reentrancy
    nested_done = true;
    m.release_ro();

    EXPECT_TRUE( nested_done.load() ) << "nested read acquire deadlocked behind a queued writer";
    EXPECT_FALSE( writer_done.load() ) << "writer should still be parked behind the outer hold";

    m.release_ro(); // release the outer hold -- only now can the writer succeed
    writer.join();
    EXPECT_TRUE( writer_done.load() );
    EXPECT_FALSE( m.is_locked() );
}

// The inverted property from WriterPreferringContract: a NEW reader (a different
// thread than the one already holding the read side) MUST be admitted even once a
// writer is queued -- that is what "reader preferring" means. Trait-gated: only
// reader_preferring_futex_rw_mutex guarantees this on every platform;
// reader_preferring_rw_mutex only guarantees it where glibc's NP rwlock-kind
// extensions back it with a genuinely reader-preferring OS primitive (see
// mutex_test_traits.hpp / rrw_mutex.hpp).
TYPED_TEST( ReaderPreferringContract, NewReaderAdmittedDespiteQueuedWriter )
{
    if constexpr ( !test::mutex_traits<TypeParam>::admits_reader_with_writer_queued )
    {
        GTEST_SKIP() << "this platform's reader_preferring_rw_mutex is still writer-preferring "
                         "underneath (basic_rrw_mutex, rrw_mutex.hpp) -- new-reader-admission "
                         "under a queued writer does not apply here";
    }
    else
    {
        TypeParam m;
        std::atomic<bool> reader1_held    { false };
        std::atomic<bool> writer_queued   { false };
        std::atomic<bool> reader2_admitted{ false };
        std::atomic<bool> release_reader1 { false };
        std::atomic<bool> writer_done     { false };

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
            m.acquire_rw(); // blocks until reader1 releases
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
}

//==============================================================================
// GuardContract -- RAII guards / std::shared_lock interop. Only the two
// rw_mutex-derived types (rw_lock/basic_ro_lock bind to rw_mutex&/Mutex&); the
// futex_rw_mutex family is a separate hierarchy with no guard types of its own
// (see futex_rw_mutex_test.cpp's history -- it always used raw acquire/release).
//==============================================================================

template <class Mutex>
struct GuardContract : ::testing::Test {};

using GuardTypes = ::testing::Types<rw_mutex, reader_preferring_rw_mutex>;
TYPED_TEST_SUITE( GuardContract, GuardTypes, TypeNames );

TYPED_TEST( GuardContract, StdLockInterop )
{
    TypeParam m;
    {
        std::shared_lock const reader{ m };
        EXPECT_FALSE( m.try_lock() );
    }
    {
        std::unique_lock const writer{ m };
        // probe from another thread: a same-thread read attempt while holding the
        // exclusive side is the documented deadlock rw_mutex ASSERTS on in debug builds
        std::atomic<bool> read_excluded{ false };
        std::thread other{ [&]
        {
            if ( m.try_lock_shared() )
            {
                m.unlock_shared();
            }
            else
            {
                read_excluded = true;
            }
        } };
        other.join();
        EXPECT_TRUE( read_excluded.load() );
    }
    {
        std::unique_lock writer{ m, std::try_to_lock };
        EXPECT_TRUE( writer.owns_lock() );
    }
}

TYPED_TEST( GuardContract, MovedFromGuardDoesNotRelease )
{
    TypeParam m;
    {
        basic_ro_lock<TypeParam> outer{ m };
        basic_ro_lock<TypeParam> const stolen{ std::move( outer ) }; // outer must not release on destruction
        EXPECT_FALSE( m.try_acquire_rw() );                          // still read-held via `stolen`
    }
    EXPECT_TRUE( m.try_acquire_rw() );
    m.release_rw();
}

//==============================================================================
// FutexOnlyContract -- properties the two futex_rw_mutex.hpp types already
// shared with each other, kept scoped to just those two (see this file's
// header comment for why this is not folded into MutexContract).
//==============================================================================

template <class Mutex>
struct FutexOnlyContract : ::testing::Test {};

using FutexOnlyTypes = ::testing::Types<futex_rw_mutex, reader_preferring_futex_rw_mutex>;
TYPED_TEST_SUITE( FutexOnlyContract, FutexOnlyTypes, TypeNames );

// Regression: futex's std::atomic base has a deleted copy ctor, which without
// these types' own explicit copy/move ctors (mirroring rw_mutex's) would silently
// delete the copy/move ctors of ANY type embedding one as a member (e.g. rama's
// BitmapIndex on Apple) -- not a compile error at this point, just a quietly
// non-copyable/non-movable enclosing type discovered far away at its own use site.
TYPED_TEST( FutexOnlyContract, IsCopyableAndMovable )
{
    static_assert( std::is_copy_constructible_v<TypeParam> );
    static_assert( std::is_move_constructible_v<TypeParam> );

    struct Owner { TypeParam mutex; };
    static_assert( std::is_copy_constructible_v<Owner> );
    static_assert( std::is_move_constructible_v<Owner> );

    Owner original;
    Owner copied{ original }; // must not deadlock/assert: source is dormant
    Owner moved{ std::move( original ) };
    EXPECT_FALSE( copied.mutex.is_locked() );
    EXPECT_FALSE( moved.mutex.is_locked() );
}

// Stress/invariant test: many reader and writer threads hammer a shared counter
// guarded by the mutex; an exclusive holder must observe itself as the sole
// holder (no concurrent reader or second writer), and readers must never observe
// a writer concurrently. Also exercises that every writer eventually gets in (no
// permanent starvation / no deadlock) under sustained contention -- deliberately
// NOT run against reader_preferring_rw_mutex, see this file's header comment.
TYPED_TEST( FutexOnlyContract, StressMutualExclusionAndProgress )
{
    TypeParam m;
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
