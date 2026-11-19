////////////////////////////////////////////////////////////////////////////////
/// Process-wide in-flight work counter for psi::sweater shops.
///
/// Every `fire_and_forget` / `fire_with_after` increments before the work is
/// queued and decrements after the work functor returns (worker thread for
/// compute; loop thread for libuv after-callback). Enables drain barriers for
/// DbOpQueue deferred erase, standalone replay daemons, and sanitizer runs
/// without ad-hoc per-consumer counters.
////////////////////////////////////////////////////////////////////////////////
#pragma once
//------------------------------------------------------------------------------
#include "threading/hardware_concurrency.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
//------------------------------------------------------------------------------
namespace psi::sweater::detail
{
//------------------------------------------------------------------------------

// On a cache line of its own: every fired item writes the counter twice, from
// two threads, and whatever shares its line is taken from both each time. Left
// to the linker it lands among the read-mostly globals of the pools (e.g. the
// spin counts every worker reads when it runs out of work), and which of them
// it lands next to changes with the sizes of unrelated objects.
struct alignas( thrd_lite::destructive_interference_size ) in_flight_counter
{
    std::atomic<std::size_t> value{ 0 };
};

inline in_flight_counter g_in_flight;

inline void in_flight_inc() noexcept
{
    g_in_flight.value.fetch_add( 1, std::memory_order_acq_rel );
}

inline void in_flight_dec() noexcept
{
    g_in_flight.value.fetch_sub( 1, std::memory_order_acq_rel );
}

[[ nodiscard ]] inline std::size_t in_flight_count() noexcept
{
    return g_in_flight.value.load( std::memory_order_acquire );
}

/// Spin until no sweater work is in flight, or `timeout` elapses.
[[ nodiscard ]] inline bool wait_until_idle(
    std::chrono::steady_clock::duration timeout = std::chrono::minutes{ 5 }
) noexcept
{
    auto const deadline{ std::chrono::steady_clock::now() + timeout };
    while ( in_flight_count() != 0 )
    {
        if ( std::chrono::steady_clock::now() >= deadline )
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

//------------------------------------------------------------------------------
} // namespace psi::sweater::detail
//------------------------------------------------------------------------------
