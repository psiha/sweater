////////////////////////////////////////////////////////////////////////////////
///
/// \file rw_preference.hpp
/// -----------------------
///
/// (c) Copyright Domagoj Saric 2026.
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
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// Which side a contended rw_mutex favours when both a reader and a writer are
// waiting. This is a *compile-time* choice (an NTTP on the platform rw_mutex
// implementations), not a runtime ctor flag: the preference is baked into the
// underlying OS primitive at construction (a static initializer or an attr
// object picked once), so making it a type-level parameter costs nothing per
// acquire/release and lets callers select it with a type alias instead of a
// branch.
//
//   - writer_preferring: a queued writer blocks newly-arriving readers (and
//     other writers) until it runs. Prevents writer starvation under a heavy
//     read load, at the cost of a single long-running reader holding off a
//     writer, and any writer blocking every reader that arrives after it --
//     see rrw_mutex.hpp for the resulting nested-read hazard on non-recursive
//     writer-preferring locks.
//   - reader_preferring: newly-arriving readers are allowed to proceed even
//     with a writer queued, so a thread that already holds the read side can
//     safely re-acquire it (nested reads are natively deadlock-free -- no
//     rrw_mutex-style per-thread hold tracking needed). The tradeoff is
//     writer starvation under sustained read load.
//
// Platform support is asymmetric and is reported by rw_mutex_traits (defined
// alongside each backend's rw_mutex): POSIX (glibc NP rwlock-kind extensions)
// can offer both preferences; Windows SRWLOCK cannot be steered -- it is
// undocumented but empirically writer-favouring (see rrw_mutex.hpp) and there
// is no API to request reader preference. A caller that only needs safe
// nested reads (not writer starvation avoidance) and targets a platform with
// reader_preferring support can use it directly instead of rrw_mutex.
enum class rw_preference : bool
{
    writer_preferring,
    reader_preferring,
};

// Primary template intentionally left undefined: every concrete rw_mutex type
// (per backend, per preference) must specialize this. Referencing an
// unspecialized rw_mutex_traits<M> is a compile error naming the missing
// specialization, which is the point -- e.g. instantiating a
// reader-preferring mutex on a backend that cannot support it should fail to
// name a type at all (see posix/rw_mutex.hpp) rather than silently degrade.
template <class RWMutex>
struct rw_mutex_traits;

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
