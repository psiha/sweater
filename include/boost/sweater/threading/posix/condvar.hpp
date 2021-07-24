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

#include <boost/config_ex.hpp>

#include <mutex>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

class pthread_condition_variable
{
public:
    pthread_condition_variable() noexcept BOOST_NOTHROW_LITE : cv_( PTHREAD_COND_INITIALIZER ) {}
   ~pthread_condition_variable() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_cond_destroy( &cv_ ) == 0 ); }

    pthread_condition_variable( pthread_condition_variable && other ) noexcept : cv_{ other.cv_ } { other.cv_ = PTHREAD_COND_INITIALIZER; }
    pthread_condition_variable( pthread_condition_variable const & ) = delete;

    void notify_all() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_cond_broadcast( &cv_ ) == 0 ); }
    void notify_one() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_cond_signal   ( &cv_ ) == 0 ); }

    void wait( std::unique_lock<pthread_mutex> & lock   ) noexcept BOOST_NOTHROW_LITE { wait( *lock.mutex() ); }
    void wait( pthread_mutex & mutex /*must be locked*/ ) noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_cond_wait( &cv_, &mutex.mutex_ ) == 0 ); }

private:
    pthread_cond_t cv_;
}; // class pthread_condition_variable

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
