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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
//------------------------------------------------------------------------------
namespace psi::sweater::detail
{
//------------------------------------------------------------------------------

inline std::atomic<std::size_t> g_in_flight{ 0 };

inline void in_flight_inc() noexcept
{
    g_in_flight.fetch_add( 1, std::memory_order_acq_rel );
}

inline void in_flight_dec() noexcept
{
    g_in_flight.fetch_sub( 1, std::memory_order_acq_rel );
}

[[ nodiscard ]] inline std::size_t in_flight_count() noexcept
{
    return g_in_flight.load( std::memory_order_acquire );
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
