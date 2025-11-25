////////////////////////////////////////////////////////////////////////////////
///
/// \file rw_lock.hpp
/// -----------------
///
/// (c) Copyright Domagoj Saric.
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
#ifdef _WIN32
#   include "windows/rw_mutex.hpp"
#else
#   include "posix/rw_mutex.hpp"
#endif

#include <memory>
#include <utility>
//------------------------------------------------------------------------------
namespace boost::thrd_lite
{
//------------------------------------------------------------------------------

class [[ clang::trivial_abi ]] ro_lock
{
public:
    constexpr ro_lock() noexcept = default;
    ro_lock( rw_mutex & mutex ) noexcept : p_mutex_{ &mutex } {                 p_mutex_->acquire_ro(); }
   ~ro_lock(                  ) noexcept                      { if ( p_mutex_ ) p_mutex_->release_ro(); }
    ro_lock( ro_lock const &  ) = delete;
    ro_lock( ro_lock && other ) noexcept : p_mutex_{ std::exchange( other.p_mutex_, nullptr ) } {}

    ro_lock & operator=( ro_lock && other ) noexcept
    {
        std::destroy_at( this );
        return *std::construct_at( this, std::move( other ) );
    }

private:
    rw_mutex * p_mutex_{};
}; // class ro_lock


class [[ clang::trivial_abi ]] rw_lock
{
public:
    constexpr rw_lock() noexcept = default;
    rw_lock( rw_mutex & mutex ) noexcept : p_mutex_{ &mutex } {                 p_mutex_->acquire_rw(); }
   ~rw_lock(                  ) noexcept                      { if ( p_mutex_ ) p_mutex_->release_rw(); }
    rw_lock( rw_lock const &  ) = delete;
    rw_lock( rw_lock && other ) noexcept : p_mutex_{ std::exchange( other.p_mutex_, nullptr ) } {}

private:
    rw_mutex * p_mutex_;
}; // class rw_lock

//------------------------------------------------------------------------------
} // namespace boost::thrd_lite
//------------------------------------------------------------------------------
