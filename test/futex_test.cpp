//==============================================================================
// Low-level tests for psi::thrd_lite::futex's bitset-aware wait_if_equal/wake_all
// overloads (futex.hpp). Verifies the actual generic-with-fallback contract: on
// Linux (FUTEX_WAIT_BITSET/FUTEX_WAKE_BITSET), a wake_all(bits) call only wakes
// waiters that parked with a matching listen_bits; everywhere else the bitset
// argument is a documented no-op and wake_all(bits) behaves exactly like plain
// wake_all() (wakes every waiter on this word, regardless of bits).
//==============================================================================

#include <psi/sweater/threading/futex.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

TEST( Futex, WakeAllBitsetTargeting )
{
    futex f{ 0 };
    constexpr futex::value_type bits_a{ 0b01 };
    constexpr futex::value_type bits_b{ 0b10 };

    std::atomic<bool> a_parked{ false }, a_woken{ false };
    std::atomic<bool> b_parked{ false }, b_woken{ false };

    // Neither thread's wait ever races a real value change (f stays 0 throughout) --
    // only an explicit wake_all() call below can unpark either one.
    std::thread ta{ [&] { a_parked = true; f.wait_if_equal( 0, bits_a ); a_woken = true; } };
    std::thread tb{ [&] { b_parked = true; f.wait_if_equal( 0, bits_b ); b_woken = true; } };

    while ( !a_parked.load() || !b_parked.load() )
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } ); // let both actually park

    f.wake_all( bits_a ); // must wake ONLY a matching listener

    std::this_thread::sleep_for( std::chrono::milliseconds{ 100 } );
    EXPECT_TRUE( a_woken.load() ) << "wake_all(bits_a) failed to wake the matching (bits_a) waiter";
#ifdef __linux__
    EXPECT_FALSE( b_woken.load() )
        << "wake_all(bits_a) woke a non-matching (bits_b) waiter -- Linux FUTEX_WAKE_BITSET should filter by bitset";
#else
    // No real bitset backend here (Windows WaitOnAddress / Apple __ulock_wait /
    // Emscripten): the argument is documented as ignored, so this degrades to a
    // plain wake-everyone -- see futex.hpp's design-doc comment.
    EXPECT_TRUE( b_woken.load() )
        << "expected wake_all(bits_a) to behave like plain wake_all() on this backend (bitset argument ignored)";
#endif

    f.wake_all( bits_b ); // wake tb too if it isn't already, so join() below can't hang
    ta.join();
    tb.join();
    EXPECT_TRUE( a_woken.load() );
    EXPECT_TRUE( b_woken.load() );
}

TEST( Futex, WaitIfEqualDefaultBitsetStillWorks )
{
    // The pre-existing (no-bitset-argument) call sites throughout this codebase must
    // keep compiling and behaving exactly as before -- default listen_bits/wake_bitset
    // == futex::all_bits, which matches anything.
    futex f{ 0 };
    std::atomic<bool> woken{ false };
    std::thread t{ [&] { f.wait_if_equal( 0 ); woken = true; } };
    std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } ); // let it park
    EXPECT_FALSE( woken.load() );
    f.wake_all();
    t.join();
    EXPECT_TRUE( woken.load() );
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
