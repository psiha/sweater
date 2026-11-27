////////////////////////////////////////////////////////////////////////////////
///
/// \file condvar.cpp
/// -----------------
///
/// (c) Copyright Domagoj Saric 2016 - 2026.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "condvar.hpp"

#include <boost/assert.hpp>

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <limits>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

namespace
{
    // The timeout added to ts, saturated instead of overflowing.
    ::timespec add( ::timespec ts, std::chrono::milliseconds const timeout ) noexcept
    {
        using sec_t = decltype( ts.tv_sec );
        auto const ms  { std::max<std::chrono::milliseconds::rep>( timeout.count(), 0 ) };
        auto const secs{ ms / 1000 };
        ts.tv_nsec += static_cast<long>( ms % 1000 ) * 1'000'000L;
        if ( ts.tv_nsec >= 1'000'000'000L )
        {
            ts.tv_nsec -= 1'000'000'000L;
            ++ts.tv_sec;
        }
        auto constexpr max_sec{ std::numeric_limits<sec_t>::max() };
        if ( secs > static_cast<std::chrono::milliseconds::rep>( max_sec - ts.tv_sec ) )
            ts.tv_sec = max_sec;
        else
            ts.tv_sec += static_cast<sec_t>( secs );
        return ts;
    }

#if !defined( PSI_THRD_LITE_CONDVAR_RELATIVE_NP )
    ::timespec monotonic_deadline( std::chrono::milliseconds const timeout ) noexcept
    {
        ::timespec now;
        BOOST_VERIFY( ::clock_gettime( CLOCK_MONOTONIC, &now ) == 0 );
        return add( now, timeout );
    }
#endif
} // anonymous namespace

bool pthread_condition_variable::wait( mutex_t & mutex /*must be locked*/, std::chrono::milliseconds const timeout ) noexcept
{
#if   defined( PSI_THRD_LITE_CONDVAR_RELATIVE_NP )
    auto const relative{ add( ::timespec{}, timeout ) };
    auto const result  { ::pthread_cond_timedwait_relative_np( &cv_, &mutex.mutex_, &relative ) };
#elif defined( PSI_THRD_LITE_CONDVAR_CLOCKWAIT )
    auto const deadline{ monotonic_deadline( timeout ) };
    auto const result  { ::pthread_cond_clockwait( &cv_, &mutex.mutex_, CLOCK_MONOTONIC, &deadline ) };
#else
    auto const deadline{ monotonic_deadline( timeout ) };
    auto const result  { ::pthread_cond_timedwait( &cv_, &mutex.mutex_, &deadline ) };
#endif
    BOOST_ASSERT( ( result == 0 ) || ( result == ETIMEDOUT ) );
    return result == 0;
}

#if defined( PSI_THRD_LITE_CONDVAR_MONOTONIC_ATTR )
void pthread_condition_variable::init( pthread_cond_t & cv ) noexcept
{
    ::pthread_condattr_t attr;
    BOOST_VERIFY( ::pthread_condattr_init    ( &attr                  ) == 0 );
    BOOST_VERIFY( ::pthread_condattr_setclock( &attr, CLOCK_MONOTONIC ) == 0 );
    BOOST_VERIFY( ::pthread_cond_init        ( &cv  , &attr           ) == 0 );
    BOOST_VERIFY( ::pthread_condattr_destroy ( &attr                  ) == 0 );
}
#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
