////////////////////////////////////////////////////////////////////////////////
///
/// \file read_recursion_registry.hpp
/// ---------------------------------
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
#include <boost/assert.hpp>

#include <cstddef>
#include <cstdint>
#include <tuple> // std::ignore
#ifndef NDEBUG
#include <vector>
#endif

// PSI_NOEXCEPT_EXCEPT_BADALLOC is normally supplied project-wide through the psi
// build options (-D...): `noexcept` under a non-failing (overcommit-Full) allocator,
// empty (may-throw) otherwise. The reentrant read acquire below allocates to track a
// hold, so it carries that annotation. Provide the conservative may-throw fallback so
// this header is usable in a standalone sweater build that does not pull in the psi
// build options.
#ifndef PSI_NOEXCEPT_EXCEPT_BADALLOC
#   define PSI_NOEXCEPT_EXCEPT_BADALLOC
#endif
//------------------------------------------------------------------------------
namespace psi::thrd_lite::detail
{
//------------------------------------------------------------------------------

// One record of "this thread currently holds the read side of `mutex`, with recursion
// depth `depth`". Keyed by object identity (void const *) so the registry stays fully
// decoupled from rw_mutex.
struct read_hold { void const * mutex; std::uint32_t depth; };

// Per-thread record of the read locks the calling thread currently holds, with a
// per-mutex recursion depth. The container is a template parameter (`HeldVec`, a
// vector-like of read_hold) so callers pick the storage / allocation policy: a
// dependency-light std::vector for standalone use, or an SBO small_vector to keep the
// shallow common case allocation-free.
//
// UNBOUNDED on purpose: a single read operation can legitimately hold read locks on
// many DISTINCT mutexes at once (e.g. a per-element lock over a user-supplied, uncapped
// collection), so a fixed cap that overflowed would silently fall through to an
// untracked acquire -- reintroducing the very recursion hang this exists to prevent,
// and making release ambiguous (double-release of the OS lock => UB).
template <class HeldVec>
class read_recursion_registry
{
public:
    // Returns true iff this is the FIRST hold on `m` by this thread, i.e. the caller
    // must now take the OS read lock. Nested holds just bump the depth.
    [[ nodiscard ]] bool enter( void const * const m ) PSI_NOEXCEPT_EXCEPT_BADALLOC
    {
        for ( auto & e : held_ )
        {
            if ( e.mutex == m )
            {
                ++e.depth;
                return false;
            }
        }
        held_.emplace_back( m, std::uint32_t{ 1 } );
        return true;
    }

    // Returns true iff this was the LAST hold on `m` by this thread, i.e. the caller
    // must now drop the OS read lock.
    [[ nodiscard ]] bool leave( void const * const m ) noexcept
    {
        for ( typename HeldVec::size_type i{ 0 }; i < held_.size(); ++i )
        {
            if ( held_[ i ].mutex == m )
            {
                if ( --held_[ i ].depth == 0 )
                {
                    held_[ i ] = held_.back();
                    held_.pop_back();
                    return true;
                }
                return false;
            }
        }
        // Unreachable for balanced same-thread use. Never drop an untracked OS lock
        // (a double-release of the OS rwlock is UB); leak-safe no-op instead.
        BOOST_ASSERT_MSG( false, "read_recursion_registry: release with no matching acquire on this thread" );
        return false;
    }

    // try-acquire helper: bump iff `m` is already held by this thread (a reentrant try
    // succeeds without touching the OS). Returns true iff it was held (and was bumped).
    [[ nodiscard ]] bool bump_if_held( void const * const m ) noexcept
    {
        for ( auto & e : held_ )
        {
            if ( e.mutex == m )
            {
                ++e.depth;
                return true;
            }
        }
        return false;
    }

    // Guarantee capacity for one more hold so a subsequent note_first cannot fail. Called
    // BEFORE the OS try-acquire: any allocation failure then happens while no OS lock is
    // held, so nothing can leak.
    void reserve_one() PSI_NOEXCEPT_EXCEPT_BADALLOC { held_.reserve( held_.size() + 1 ); }

    // Record a fresh (depth 1) hold after a successful OS try-acquire by this thread.
    // Requires a preceding reserve_one() -- with capacity guaranteed the push cannot throw.
    void note_first( void const * const m ) noexcept { held_.emplace_back( m, std::uint32_t{ 1 } ); }

    [[ nodiscard ]] static read_recursion_registry & tls() noexcept
    {
        static thread_local read_recursion_registry instance;
        return instance;
    }

private:
    HeldVec held_;
}; // class read_recursion_registry


// ---------------------------------------------------------------------------
// Debug-only recursion detection for the NON-recursive rw_mutex (see rw_mutex.hpp).
// rw_mutex wraps a writer-preferring, non-recursive OS primitive: recursively taking
// its read side while a writer is queued is a documented hang (POSIX
// pthread_rwlock_rdlock "may deadlock"; Windows SRWLOCK "cannot be acquired
// recursively"). These hooks make that misuse fail loudly at the first nested acquire
// instead of parking at 0% CPU until a timeout. They are a complete no-op (and incur
// no storage) in release builds.
// ---------------------------------------------------------------------------
#ifndef NDEBUG
[[ gnu::cold ]] inline void on_ro_acquire( void const * const m )
{
    BOOST_ASSERT_MSG
    (
        read_recursion_registry<std::vector<read_hold>>::tls().enter( m ),
        "recursive read-lock on a non-recursive psi::thrd_lite::rw_mutex -- use rrw_mutex for reentrant reads"
    );
}
inline void on_ro_release( void const * const m ) noexcept
{
    std::ignore = read_recursion_registry<std::vector<read_hold>>::tls().leave( m );
}
#else
inline void on_ro_acquire( void const * ) noexcept {}
inline void on_ro_release( void const * ) noexcept {}
#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite::detail
//------------------------------------------------------------------------------
