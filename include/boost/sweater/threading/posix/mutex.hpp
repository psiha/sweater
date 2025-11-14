////////////////////////////////////////////////////////////////////////////////
///
/// \file mutex.hpp
/// ---------------
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
#pragma once
//------------------------------------------------------------------------------
#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#include <pthread.h>
//------------------------------------------------------------------------------
namespace boost::thrd_lite
{
//------------------------------------------------------------------------------

class pthread_condition_variable;

class [[ clang::trivial_abi ]] pthread_mutex
{
private:
#if !defined( NDEBUG ) && defined( PTHREAD_ERRORCHECK_MUTEX_INITIALIZER )
    static pthread_mutex_t constexpr initializer = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
#else
    static pthread_mutex_t constexpr initializer = PTHREAD_MUTEX_INITIALIZER;
#endif // NDEBUG
public:
    pthread_mutex() noexcept = default;
   ~pthread_mutex() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_mutex_destroy( &mutex_ ) == 0 ); }

    explicit // allow copy so as to enable use compiler generated constructors/functions for types that contain mutex members
    pthread_mutex( [[ maybe_unused ]] pthread_mutex const &  other ) noexcept { BOOST_ASSERT_MSG( !is_locked(), "Copy allowed only for dormant mutexes" ); }
    pthread_mutex( [[ maybe_unused ]] pthread_mutex       && other ) noexcept { BOOST_ASSERT_MSG( !is_locked(), "Relocation allowed only for dormant mutexes" ); }

    pthread_mutex & operator=( [[ maybe_unused ]] pthread_mutex && other ) noexcept
    {
        BOOST_ASSERT_MSG( !is_locked() && !other.is_locked(), "Relocation allowed only for dormant mutexes" );
        return *this;
    }

    void   lock() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_mutex_lock  ( &mutex_ ) == 0 ); }
    void unlock() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_mutex_unlock( &mutex_ ) == 0 ); }

    bool try_lock() noexcept BOOST_NOTHROW_LITE { return pthread_mutex_trylock( &mutex_ ) == 0; }

    // debugging aid
    bool is_locked() const noexcept
    {
        auto & mtbl{ const_cast<pthread_mutex &>( *this ) };
        if ( mtbl.try_lock() )
        {
            mtbl.unlock();
            return false;
        }
        return true;
    }

private: friend class pthread_condition_variable;
    pthread_mutex_t mutex_{ initializer };
}; // class pthread_mutex

//------------------------------------------------------------------------------
} // namespace boost::thrd_lite
//------------------------------------------------------------------------------
