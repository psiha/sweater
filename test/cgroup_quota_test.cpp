////////////////////////////////////////////////////////////////////////////////
//
// The cgroup CPU quota parsing is pure text processing: it is tested with
// literal inputs, without a real cgroup. The implementation file is compiled
// into this executable with the cgroup support enabled (it is opt-in).
//
////////////////////////////////////////////////////////////////////////////////
#ifndef PSI_SWEATER_DOCKER_LIMITS
#   define PSI_SWEATER_DOCKER_LIMITS 1
#endif
#include <psi/sweater/threading/hardware_concurrency.cpp>

#include <gtest/gtest.h>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{

TEST( CgroupQuota, CpuMax )
{
    EXPECT_EQ( cpus_from_cpu_max( "max 100000\n"    ), -1 ); // unconstrained
    EXPECT_EQ( cpus_from_cpu_max( "100000 100000\n" ),  1 );
    EXPECT_EQ( cpus_from_cpu_max( "200000 100000\n" ),  2 );
    EXPECT_EQ( cpus_from_cpu_max( "150000 100000\n" ),  2 ); // fractional quotas are rounded
    EXPECT_EQ( cpus_from_cpu_max( "50000 100000\n"  ),  1 ); // never below one CPU
    EXPECT_EQ( cpus_from_cpu_max( "800000 100000"   ),  8 );
    EXPECT_EQ( cpus_from_cpu_max( "100000 0"        ), -1 ); // malformed period
}

TEST( CgroupQuota, QuotaPeriodPair )
{
    EXPECT_EQ( cpus_from_quota( -1      , 100000 ), -1 ); // v1: -1 means unconstrained
    EXPECT_EQ( cpus_from_quota( 400000  , 100000 ),  4 );
    EXPECT_EQ( cpus_from_quota( 1000    , 100000 ),  1 );
}

} // namespace psi::thrd_lite
