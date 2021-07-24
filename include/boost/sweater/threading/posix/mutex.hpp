////////////////////////////////////////////////////////////////////////////////
///
/// \file mutex.hpp
/// ---------------
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
#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#include <pthread.h>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

class pthread_condition_variable;

class pthread_mutex
{
public:
    pthread_mutex() noexcept BOOST_NOTHROW_LITE
#if !defined( NDEBUG ) && defined( PTHREAD_ERRORCHECK_MUTEX_INITIALIZER )
    : mutex_( PTHREAD_ERRORCHECK_MUTEX_INITIALIZER ) {}
#else
    : mutex_( PTHREAD_MUTEX_INITIALIZER ) {}
#endif // NDEBUG
   ~pthread_mutex() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_mutex_destroy( &mutex_ ) == 0 ); }

    pthread_mutex( pthread_mutex && other ) noexcept : mutex_( other.mutex_ ) { other.mutex_ = PTHREAD_MUTEX_INITIALIZER; }
    pthread_mutex( pthread_mutex const & ) = delete;

    void   lock() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_mutex_lock  ( &mutex_ ) == 0 ); }
    void unlock() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_mutex_unlock( &mutex_ ) == 0 ); }

    bool try_lock() noexcept BOOST_NOTHROW_LITE { return pthread_mutex_trylock( &mutex_ ) == 0; }

private: friend class pthread_condition_variable;
    pthread_mutex_t mutex_;
}; // class pthread_mutex

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
