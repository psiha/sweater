////////////////////////////////////////////////////////////////////////////////
///
/// \file rw_mutex.hpp
/// ------------------
///
/// (c) Copyright Domagoj Saric 2024.
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

#include <pthread.h>
#ifndef NDEBUG
#include <cstring>
#endif
//------------------------------------------------------------------------------
namespace boost::thrd_lite
{
//------------------------------------------------------------------------------

class [[ clang::trivial_abi ]] rw_mutex
{
public:
    rw_mutex(                   ) noexcept { BOOST_VERIFY( pthread_rwlock_init( &lock_, nullptr ) == 0 ); }
    rw_mutex( rw_mutex && other ) noexcept : lock_{ other.lock_ } { BOOST_VERIFY( pthread_rwlock_init( &other.lock_, nullptr ) == 0 ); } // OSX: move not supported while locked
    rw_mutex( rw_mutex const &  ) = delete ;
   ~rw_mutex(                   ) noexcept { BOOST_VERIFY( pthread_rwlock_destroy( &lock_ ) == 0 ); }

    rw_mutex & operator=( [[ maybe_unused ]] rw_mutex && other ) noexcept
    {
#   ifndef NDEBUG
        // this operation makes sense only for dormant mutexes
        rw_mutex dormant;
        BOOST_ASSERT( std::memcmp(  this , &dormant, sizeof( *this ) ) == 0 );
        BOOST_ASSERT( std::memcmp( &other, &dormant, sizeof( *this ) ) == 0 );
#   endif
        return *this;
    }

    void acquire_ro() noexcept { BOOST_VERIFY( pthread_rwlock_rdlock( &lock_ ) == 0 ); }
    void release_ro() noexcept { BOOST_VERIFY( pthread_rwlock_unlock( &lock_ ) == 0 ); }

    void acquire_rw() noexcept { BOOST_VERIFY( pthread_rwlock_wrlock( &lock_ ) == 0 ); }
    void release_rw() noexcept { release_ro(); }

    bool try_acquire_ro() noexcept { return pthread_rwlock_tryrdlock( &lock_ ) == 0; }
    bool try_acquire_rw() noexcept { return pthread_rwlock_trywrlock( &lock_ ) == 0; }

public: // std::shared_lock interface
    void   lock() noexcept { acquire_rw(); }
    void unlock() noexcept { release_rw(); }

    bool try_lock() noexcept { return try_acquire_rw(); }


    void   lock_shared() noexcept { acquire_ro(); }
    void unlock_shared() noexcept { release_ro(); }

    bool try_lock_shared() noexcept { return try_acquire_ro(); }

private:
    pthread_rwlock_t lock_;
}; // class rw_mutex

//------------------------------------------------------------------------------
} // namespace boost::thrd_lite
//------------------------------------------------------------------------------
