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
// waiting. Selected once, at construction (posix/rw_mutex.hpp's protected ctor
// overloads below), not per acquire/release -- writer_preferring and
// reader_preferring rw_mutex instances are the SAME type (rw_mutex, or a trivial
// derived class that adds no methods a caller would ever call differently),
// differing only in which underlying OS primitive attributes the ctor picked.
// Guards (rw_lock) and std::shared_lock usage are therefore identical either way;
// only the read-side RAII guard needs to name the concrete type (see
// reader_preferring_rw_mutex's shadowed acquire_ro/release_ro in
// posix/rw_mutex.hpp, and rro_lock in rrw_mutex.hpp), same as it already did for
// rrw_mutex before any of this existed.
//
// Tag TYPES, not an enum: the ctor overload is picked by the compiler (ordinary
// overload resolution), not compared at runtime. writer_preferring_t's overload
// exists on every platform (every backend can build a writer-preferring-or-best-
// effort-default lock); reader_preferring_t's overload is declared ONLY where the
// underlying OS primitive can actually be steered (posix/rw_mutex.hpp, gated on
// glibc's NP rwlock-kind extensions). Attempting to build a reader-preferring
// rw_mutex where that overload does not exist is then a plain "no matching
// constructor" COMPILE error -- no runtime check, no assert, needed to catch it.
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
// Platform support is asymmetric: POSIX with glibc's NP rwlock-kind extensions
// (Linux) can offer both preferences on the real primitive; everywhere else
// (Windows SRWLOCK, macOS pthread_rwlock) there is no API to request reader
// preference, so reader_preferring_rw_mutex there means something weaker --
// "safe nested reads" via the same per-thread hold-tracking rrw_mutex always
// used, not genuine OS-level reader preference against a newly-arriving reader
// on another thread. See rrw_mutex.hpp for exactly which mechanism backs
// reader_preferring_rw_mutex / rrw_mutex on a given platform.
struct writer_preferring_t {};
struct reader_preferring_t {};
inline constexpr writer_preferring_t writer_preferring{};
inline constexpr reader_preferring_t reader_preferring{};

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
