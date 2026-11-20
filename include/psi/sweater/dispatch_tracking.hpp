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

// The count is kept as two totals, each on a cache line of its own: the threads
// that fire items write only `fired`, the threads that run them only
// `completed`. A fired item then moves no line between the firing thread and
// the thread that runs it, and with one thread on each side neither line moves
// at all; only a reader of the count takes both.
struct alignas( thrd_lite::destructive_interference_size ) in_flight_total
{
    std::atomic<std::size_t> value{ 0 };
};

inline in_flight_total g_fired;
inline in_flight_total g_completed;

inline void in_flight_inc() noexcept
{
    g_fired.value.fetch_add( 1, std::memory_order_acq_rel );
}

inline void in_flight_dec() noexcept
{
    g_completed.value.fetch_add( 1, std::memory_order_acq_rel );
}

[[ nodiscard ]] inline std::size_t in_flight_count() noexcept
{
    // `completed` is read first: `fired`, read after it, can only have grown,
    // so the result is never below what a single counter would have held when
    // `completed` was read. An item that is counted as fired only after it was
    // queued can complete before it is counted; the count then wraps high
    // until it is counted, as a single counter did.
    auto const completed{ g_completed.value.load( std::memory_order_acquire ) };
    auto const fired    { g_fired    .value.load( std::memory_order_acquire ) };
    return fired - completed;
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
