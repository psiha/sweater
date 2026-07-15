//==============================================================================
// Test-only trait distinguishing the one property that genuinely differs, per
// platform, between the two "reader-preferring" mutex types -- whether a NEW
// reader (a different thread than the one already holding the read side) is
// admitted once a writer is queued.
//
// reader_preferring_futex_rw_mutex: unconditional true everywhere (its
// admission policy is a fixed CAS condition on the futex state word, see
// futex_rw_mutex.hpp's class doc comment).
//
// reader_preferring_rw_mutex: true only where glibc's NP rwlock-kind
// extensions make it the genuinely reader-preferring OS primitive
// (posix/rw_mutex.hpp). Everywhere else it is rrw_mutex.hpp's basic_rrw_mutex
// -- per-thread hold-collapsing that makes nested SAME-THREAD reads safe, but
// is "still writer-preferring underneath": a queued writer still blocks a new
// reader there. This macro gate mirrors the one rw_preference_test.cpp uses.
//==============================================================================

#pragma once

#include <psi/sweater/threading/futex_rw_mutex.hpp>
#include <psi/sweater/threading/rrw_mutex.hpp>
#include <psi/sweater/threading/rw_mutex.hpp>

//------------------------------------------------------------------------------
namespace psi::thrd_lite::test
{
//------------------------------------------------------------------------------

template <class Mutex>
struct mutex_traits
{
    static constexpr bool admits_reader_with_writer_queued = false;
};

template <>
struct mutex_traits<reader_preferring_futex_rw_mutex>
{
    static constexpr bool admits_reader_with_writer_queued = true;
};

#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
template <>
struct mutex_traits<reader_preferring_rw_mutex>
{
    static constexpr bool admits_reader_with_writer_queued = true;
};
#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite::test
//------------------------------------------------------------------------------
