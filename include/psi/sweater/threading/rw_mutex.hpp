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
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// RAII read guard, templated on the mutex type so a single definition serves both the
// non-recursive rw_mutex (`ro_lock`) and the reentrant rrw_mutex (`rro_lock`, see
// rrw_mutex.hpp). It stores Mutex* (not rw_mutex*) so acquire_ro/release_ro dispatch to
// the most-derived overrides -- a base rw_mutex* would slice past rrw_mutex's reentrant
// ones. The ctor forwards the mutex's own exception specification: plain noexcept for
// the non-recursive rw_mutex, may-throw-on-bad-alloc for the reentrant rrw_mutex (whose
// acquire_ro allocates to track the hold).
template <class Mutex>
class [[ clang::trivial_abi ]] basic_ro_lock
{
public:
    constexpr basic_ro_lock() noexcept = default;
    basic_ro_lock( Mutex & mutex ) noexcept( noexcept( mutex.acquire_ro() ) ) : p_mutex_{ &mutex } {                 p_mutex_->acquire_ro(); }
   ~basic_ro_lock(              ) noexcept                                          { if ( p_mutex_ ) p_mutex_->release_ro(); }
    basic_ro_lock( basic_ro_lock const &  ) = delete;
    basic_ro_lock( basic_ro_lock && other ) noexcept : p_mutex_{ std::exchange( other.p_mutex_, nullptr ) } {}

    basic_ro_lock & operator=( basic_ro_lock && other ) noexcept
    {
        std::destroy_at( this );
        return *std::construct_at( this, std::move( other ) );
    }

private:
    Mutex * p_mutex_{};
}; // class basic_ro_lock

using ro_lock = basic_ro_lock<rw_mutex>;


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
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
