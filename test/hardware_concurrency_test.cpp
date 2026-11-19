////////////////////////////////////////////////////////////////////////////////
//
// The pool size (get_hardware_concurrency_max()) is computed once per process,
// so it can only be observed under a restricted affinity mask by a process
// that starts with that mask: the test re-launches its own executable with the
// mask applied and the child checks the value against the mask size.
//
////////////////////////////////////////////////////////////////////////////////
#include <psi/sweater/threading/hardware_concurrency.hpp>

#include "affinity_relaunch.hpp"

#include <gtest/gtest.h>
//------------------------------------------------------------------------------
namespace
{
    using namespace psi::sweater::test;

#if defined( __linux__ ) || defined( _WIN32 )
    bool probe_under_affinity( std::size_t const count )
    {
        return run_under_affinity( count, "HardwareConcurrency.ChildProbe" );
    }
#endif
} // anonymous namespace
//------------------------------------------------------------------------------

TEST( HardwareConcurrency, ChildProbe )
{
    auto const expected{ restricted_cpus() };
    if ( !expected )
        GTEST_SKIP() << "only meaningful when launched by the affinity tests";
    EXPECT_EQ( psi::thrd_lite::get_hardware_concurrency_max(), expected );
    EXPECT_EQ( static_cast<int>( psi::thrd_lite::hardware_concurrency_max ), expected );
    EXPECT_EQ( psi::thrd_lite::hardware_concurrency_current(), expected );
}

#if defined( __linux__ ) || defined( _WIN32 )
TEST( HardwareConcurrency, FollowsAffinityMask )
{
    if ( available_cpu_count() < 3 )
        GTEST_SKIP() << "needs at least 3 allowed CPUs to build a distinguishable mask";
    // A limit other than the mask (a CPU quota, where that support is built
    // in) caps the children too: the mask sizes would not be what they report.
    if ( psi::thrd_lite::get_hardware_concurrency_max() < 3 )
        GTEST_SKIP() << "the process is limited to fewer than 3 CPUs by something other than its mask";
    EXPECT_TRUE( probe_under_affinity( 1 ) );
    EXPECT_TRUE( probe_under_affinity( 2 ) );
    EXPECT_TRUE( probe_under_affinity( 3 ) );
}
#endif

TEST( HardwareConcurrency, NeverBelowOne )
{
    EXPECT_GE( psi::thrd_lite::get_hardware_concurrency_max(), 1 );
    EXPECT_GE( psi::thrd_lite::hardware_concurrency_current(), 1 );
}
