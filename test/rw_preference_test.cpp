//==============================================================================
// Type-identity sanity checks for the writer_preferring_rw_mutex /
// reader_preferring_rw_mutex split (posix/rw_mutex.hpp), specific to the glibc
// NP rwlock-kind extensions and therefore not something a portable typed test
// suite can assert on. Behavioral coverage for reader_preferring_rw_mutex
// (nested reads, new-reader-admission) now lives in rw_mutex_contract_test.cpp's
// ReaderPreferringContract, which runs on every platform (trait-gated for the
// one property -- new-reader-admission -- that only holds here under this
// same macro; see mutex_test_traits.hpp).
//
// This file only builds where reader_preferring_rw_mutex is defined this way
// (glibc NP rwlock-kind extensions); on every other platform it compiles to
// zero tests, which is expected, not a gap -- the type still exists there
// (rrw_mutex.hpp's basic_rrw_mutex), it is just not this distinct-from-
// rw_mutex, OS-kind-flag-based mechanism.
//==============================================================================

#include <psi/sweater/threading/rrw_mutex.hpp>
#include <psi/sweater/threading/rw_mutex.hpp>

#include <gtest/gtest.h>

#include <type_traits>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP

// writer_preferring_rw_mutex is rw_mutex itself; reader_preferring_rw_mutex (and so
// rrw_mutex) is a distinct type that derives from it -- both bind to a plain
// rw_mutex& (rw_lock's parameter type), so no separate write-guard type is needed.
static_assert(  std::is_same_v<writer_preferring_rw_mutex, rw_mutex> );
static_assert( !std::is_same_v<reader_preferring_rw_mutex, rw_mutex> );
static_assert(  std::is_base_of_v<rw_mutex, reader_preferring_rw_mutex> );
static_assert(  std::is_same_v<rrw_mutex, reader_preferring_rw_mutex> );

// Sanity: the two preferences are distinct types with independent state, and the
// SAME rw_lock/ro_lock-family guards work for both (no preference-specific guard
// types -- that's the point of the redesign this test pins).
TEST( ReaderPreferringRWMutex, IndependentFromWriterPreferringMutex )
{
    rw_mutex                   w;
    reader_preferring_rw_mutex r;

    rw_lock w_lock{ w };
    rw_lock r_lock{ r }; // same guard type as above -- accepts r by upcast to rw_mutex&

    EXPECT_TRUE( w.is_locked() );
    EXPECT_TRUE( r.is_locked() );
}

#endif

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
