////////////////////////////////////////////////////////////////////////////////
///
/// \file generic_semaphore.cpp
/// ---------------------------
///
/// (c) Copyright Domagoj Saric 2016 - 2021.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "semaphore.hpp"

#if PSI_SWEATER_CONDVAR_SEMAPHORE // selection made in semaphore.hpp

#include "cpp/spin_lock.hpp"

#include <algorithm>
#ifdef PSI_SWEATER_SEMA_STATS
#include <cinttypes>
#include <cstdio>
#endif
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

#ifdef PSI_SWEATER_SEMA_STATS // A/B instrumentation, mirrors futex_semaphore.cpp's
namespace
{
    std::atomic<std::uint64_t> sema_signals{ 0 }, sema_notifies{ 0 }, sema_parks{ 0 };
    struct sema_stats_printer
    {
        ~sema_stats_printer() { std::fprintf( stderr, "[SEMA condvar] signals=%" PRIu64 " notifies=%" PRIu64 " parks=%" PRIu64 "\n", sema_signals.load(), sema_notifies.load(), sema_parks.load() ); }
    } const sema_stats_printer_instance;
} // anonymous namespace
#define PSI_SEMA_COUNT( which ) which.fetch_add( 1, std::memory_order_relaxed )
#else
#define PSI_SEMA_COUNT( which )
#endif // PSI_SWEATER_SEMA_STATS

#ifndef NDEBUG
semaphore::~semaphore() noexcept
{
#if 0 // need not hold on early destruction (when workers exit before waiting)
    BOOST_ASSUME( value_   == 0 );
#endif
    BOOST_ASSUME( waiters_ == 0 );
}
#endif // !NDEBUG

void semaphore::signal( hardware_concurrency_t const count ) noexcept
{
#if PSI_SWEATER_EXACT_WORKER_SELECTION
    BOOST_ASSUME( count == 1 );
#endif // PSI_SWEATER_EXACT_WORKER_SELECTION
    PSI_SEMA_COUNT( sema_signals );
    auto const old_value{ value_.fetch_add( count, std::memory_order_release ) };
    if ( old_value > 0 )
    {
#   if 0 // for tiny work waiters_ can already increment/appear after the fetch_add
        BOOST_ASSUME( waiters_ == 0 );
#   endif // disabled
        return;
    }
#if PSI_SWEATER_EXACT_WORKER_SELECTION
    BOOST_ASSUME( waiters_ <= 1 );
    {
        std::scoped_lock<mutex> lock{ mutex_ };
        ++to_release_;
        if ( !waiters_ ) // unknown whether condvar notify can avoid syscalls when there are no waiters
            return;
    }
    PSI_SEMA_COUNT( sema_notifies );
    condition_.notify_one();
#else
    auto const to_wake{ std::min( static_cast<hardware_concurrency_t>( -old_value ), count ) };
    {
        std::scoped_lock<mutex> lock{ mutex_ };
        to_release_ += to_wake;
        if ( !waiters_ ) // unknown whether condvar notify can avoid syscalls when there are no waiters
            return;
    }
    if ( to_wake < waiters_ )
    {
        for ( auto notified{ 0U }; notified < to_wake; ++notified )
        {
            PSI_SEMA_COUNT( sema_notifies );
            condition_.notify_one();
        }
    }
    else
    {
        PSI_SEMA_COUNT( sema_notifies );
        condition_.notify_all();
    }
#endif // PSI_SWEATER_EXACT_WORKER_SELECTION
}

void semaphore::wait( std::uint32_t const spin_count ) noexcept // TODO: deduplicate generic spinning code
{
    // waiting for atomic_ref
    auto value{ value_.load( std::memory_order_acquire ) };
    for ( auto spin_try{ 0U }; spin_try < spin_count; ++spin_try )
    {
        if ( value > 0 )
        {
            if ( value_.compare_exchange_weak( value, value - 1, std::memory_order_acquire, std::memory_order_relaxed ) )
                return;
            //BOOST_ASSUME( value > 0 ); ...mrmlj...windows...concurrency?
        }
        else
        {
            nops( 8 );
            value = value_.load( std::memory_order_acquire );
        }

    }

    wait();
}

void semaphore::wait() noexcept
{
    auto const old_value{ value_.fetch_sub( 1, std::memory_order_acquire ) };
    if ( old_value > 0 )
        return;
    std::unique_lock<mutex> lock{ mutex_ };
    ++waiters_;
    while ( to_release_ == 0 ) // support spurious wakeups
    {
        PSI_SEMA_COUNT( sema_parks );
        condition_.wait( lock );
    }
    --to_release_;
    --waiters_;
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
#endif // PSI_SWEATER_CONDVAR_SEMAPHORE
