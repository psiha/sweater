//==============================================================================
// Behavior tests for the plain (non-recursive) psi::thrd_lite::rw_mutex: basic
// shared/exclusive semantics, the try interfaces, the RAII guards and the
// std::shared_lock-compatible surface.
//==============================================================================

#include <psi/sweater/threading/rw_mutex.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

TEST( RWMutex, ExclusiveExcludesEverything )
{
    rw_mutex m;
    {
        rw_lock const w{ m };
        std::atomic<int> failures{ 0 };
        std::thread other{ [&]
        {
            if ( !m.try_acquire_ro() )
            {
                ++failures;
            }
            else
            {
                m.release_ro();
            }
            if ( !m.try_acquire_rw() )
            {
                ++failures;
            }
            else
            {
                m.release_rw();
            }
        } };
        other.join();
        EXPECT_EQ( failures.load(), 2 );
    }
    // released -> available again
    EXPECT_TRUE( m.try_acquire_rw() );
    m.release_rw();
}

TEST( RWMutex, SharedAllowsConcurrentReadersAndExcludesWriters )
{
    rw_mutex m;
    {
        ro_lock const reader{ m };

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
    }
    EXPECT_TRUE( m.try_acquire_rw() );
    m.release_rw();
}

TEST( RWMutex, WriterBlocksUntilReaderReleases )
{
    rw_mutex m;
    std::atomic<bool> writer_acquired{ false };

    m.acquire_ro();
    std::thread writer{ [&]
    {
        rw_lock const w{ m };
        writer_acquired = true;
    } };

    std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } );
    EXPECT_FALSE( writer_acquired.load() ); // still parked behind the read hold
    m.release_ro();
    writer.join();
    EXPECT_TRUE( writer_acquired.load() );
}

TEST( RWMutex, StdLockInterop )
{
    rw_mutex m;
    {
        std::shared_lock const reader{ m };
        EXPECT_FALSE( m.try_lock() );
    }
    {
        std::unique_lock const writer{ m };
        EXPECT_FALSE( m.try_lock_shared() );
    }
    {
        std::unique_lock writer{ m, std::try_to_lock };
        EXPECT_TRUE( writer.owns_lock() );
    }
}

TEST( RWMutex, MovedFromGuardDoesNotRelease )
{
    rw_mutex m;
    {
        ro_lock outer{ m };
        ro_lock const stolen{ std::move( outer ) }; // outer must not release on destruction
        EXPECT_FALSE( m.try_acquire_rw() );         // still read-held via `stolen`
    }
    EXPECT_TRUE( m.try_acquire_rw() );
    m.release_rw();
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
