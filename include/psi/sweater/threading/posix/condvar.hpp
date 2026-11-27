////////////////////////////////////////////////////////////////////////////////
///
/// \file condvar.hpp
/// -----------------
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
#pragma once
//------------------------------------------------------------------------------
#include "mutex.hpp"

#include <chrono>
#include <mutex>
#include <pthread.h>
//------------------------------------------------------------------------------

// Timed waits measure against the monotonic clock (wall clock adjustments must
// not stretch or cut a timeout short). The mechanism is platform specific:
//  - Apple: pthread_cond_timedwait_relative_np (relative timeout),
//  - glibc >= 2.30 / Android API >= 30: pthread_cond_clockwait( CLOCK_MONOTONIC )
//    (keeps PTHREAD_COND_INITIALIZER),
//  - otherwise (or when PSI_THRD_LITE_CONDVAR_MONOTONIC_ATTR is predefined):
//    pthread_cond_timedwait on a condvar created with a CLOCK_MONOTONIC condattr
//    clock, which needs pthread_cond_init and so an out of line constructor.
#if defined( PSI_THRD_LITE_CONDVAR_MONOTONIC_ATTR )
    // forced
#elif defined( __APPLE__ )
#   define PSI_THRD_LITE_CONDVAR_RELATIVE_NP
#elif ( defined( __GLIBC__ ) && ( ( __GLIBC__ > 2 ) || ( ( __GLIBC__ == 2 ) && ( __GLIBC_MINOR__ >= 30 ) ) ) ) ||       ( defined( __ANDROID__ ) && ( __ANDROID_API__ >= 30 ) )
#   define PSI_THRD_LITE_CONDVAR_CLOCKWAIT
#else
#   define PSI_THRD_LITE_CONDVAR_MONOTONIC_ATTR
#endif
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

class pthread_condition_variable : public condvar_wait_until<pthread_condition_variable>
{
public:
    using mutex_t = pthread_mutex;
    using lock_t  = std::unique_lock<mutex_t>;

#ifdef PSI_THRD_LITE_CONDVAR_MONOTONIC_ATTR
    pthread_condition_variable(                                     ) noexcept; // needs pthread_cond_init (out of line)
#else
    // Inline and constant-initialisable (global/static condvars need no dynamic init).
    constexpr pthread_condition_variable(                           ) noexcept : cv_( PTHREAD_COND_INITIALIZER ) {}
#endif
    // A condition variable must have no waiters when moved: the new object is
    // freshly initialised and the source is left alone.
    pthread_condition_variable( pthread_condition_variable &&       ) noexcept : pthread_condition_variable() {}
    pthread_condition_variable( pthread_condition_variable const &  ) = delete;
   ~pthread_condition_variable(                                     ) noexcept;

    void notify_all() noexcept { BOOST_VERIFY( pthread_cond_broadcast( &cv_ ) == 0 ); }
    void notify_one() noexcept { BOOST_VERIFY( pthread_cond_signal   ( &cv_ ) == 0 ); }

    void wait( lock_t  & lock                     ) noexcept { wait( *lock.mutex() ); }
    void wait( mutex_t & mutex /*must be locked*/ ) noexcept { BOOST_VERIFY( pthread_cond_wait( &cv_, &mutex.mutex_ ) == 0 ); }

    // Returns false if the timeout elapsed, true otherwise (a notification or a
    // spurious wakeup: callers recheck their predicate, see wait_until()).
    // The timeout runs on the monotonic clock; a negative one is treated as zero.
    bool wait( lock_t  & lock                     , std::chrono::milliseconds const timeout ) noexcept { return wait( *lock.mutex(), timeout ); }
    bool wait( mutex_t & mutex /*must be locked*/ , std::chrono::milliseconds timeout ) noexcept;

private:
    pthread_cond_t cv_;
}; // class pthread_condition_variable : public condvar_wait_until<pthread_condition_variable>

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
