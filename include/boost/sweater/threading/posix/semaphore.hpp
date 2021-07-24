////////////////////////////////////////////////////////////////////////////////
///
/// \file semaphore.hpp
/// -------------------
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
#include <semaphore.h>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

class pthread_semaphore
{
public:
    pthread_semaphore() noexcept { BOOST_VERIFY( ::sem_init   ( &handle_, 0, 0 ) == 0 ); }
   ~pthread_semaphore() noexcept { BOOST_VERIFY( ::sem_destroy( &handle_       ) == 0 ); }

    pthread_semaphore( pthread_semaphore const & ) = delete;

    bool try_wait() noexcept { return ::sem_trywait( &handle_ ) == 0; }

    void wait(                                ) noexcept { BOOST_VERIFY( ::sem_wait( &handle_ ) == 0 ); }
    void wait( std::uint32_t const spin_count ) noexcept
    {
        for ( auto spin_try{ 0U }; spin_try < spin_count; ++spin_try )
        {
            if ( BOOST_LIKELY( try_wait() ) )
                return;
            nops( 8 );
        }

        wait();
    }

    void signal(                              ) noexcept { BOOST_VERIFY( ::sem_post( &handle_ ) == 0 ); }
    void signal( hardware_concurrency_t count ) noexcept { while ( count ) { signal(); --count; } }

private:
    sem_t handle_;
}; // class pthread_semaphore

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
