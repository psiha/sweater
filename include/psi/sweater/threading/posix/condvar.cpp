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
#include "../condvar.hpp"

#include <boost/assert.hpp>

#include <cerrno>
#include <ctime>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

namespace
{
    // The (clamped, so never overflowing) timeout added to ts.
    ::timespec add( ::timespec ts, std::chrono::milliseconds const timeout ) noexcept
    {
        auto const ms{ clamp_timeout_ms( timeout ) };
        ts.tv_sec  += ms / 1000;
        ts.tv_nsec += static_cast<long>( ms % 1000 ) * 1'000'000L;
        if ( ts.tv_nsec >= 1'000'000'000L )
        {
            ts.tv_nsec -= 1'000'000'000L;
            ++ts.tv_sec;
        }
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

#ifdef PSI_THRD_LITE_CONDVAR_MONOTONIC_ATTR
pthread_condition_variable::pthread_condition_variable() noexcept
{
    ::pthread_condattr_t attr;
    BOOST_VERIFY( ::pthread_condattr_init    ( &attr                  ) == 0 );
    BOOST_VERIFY( ::pthread_condattr_setclock( &attr, CLOCK_MONOTONIC ) == 0 );
    BOOST_VERIFY( ::pthread_cond_init        ( &cv_ , &attr           ) == 0 );
    BOOST_VERIFY( ::pthread_condattr_destroy ( &attr                  ) == 0 );
}
#endif
pthread_condition_variable::~pthread_condition_variable() noexcept { BOOST_VERIFY( ::pthread_cond_destroy( &cv_ ) == 0 ); }

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


//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
