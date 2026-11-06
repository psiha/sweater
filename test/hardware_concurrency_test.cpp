////////////////////////////////////////////////////////////////////////////////
//
// The pool size (get_hardware_concurrency_max()) is computed once per process,
// so it can only be observed under a restricted affinity mask by a process
// that starts with that mask: the test re-launches its own executable with the
// mask applied and the child checks the value against the mask size.
//
////////////////////////////////////////////////////////////////////////////////
#include <psi/sweater/threading/hardware_concurrency.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

#ifdef __linux__
#   include <sched.h>
#   include <sys/wait.h>
#   include <unistd.h>
#elif defined( _WIN32 )
#   include <windows.h>
#endif
//------------------------------------------------------------------------------
namespace
{
    char constexpr expected_variable[]{ "PSI_SWEATER_TEST_EXPECTED_CONCURRENCY" };

#ifdef __linux__
    std::vector<int> allowed_cpus()
    {
        std::vector<int> result;
        cpu_set_t set;
        CPU_ZERO( &set );
        if ( ::sched_getaffinity( 0, sizeof( set ), &set ) != 0 )
            return result;
        for ( int cpu{ 0 }; cpu < CPU_SETSIZE; ++cpu )
            if ( CPU_ISSET( cpu, &set ) )
                result.emplace_back( cpu );
        return result;
    }

    std::size_t available_cpu_count() { return allowed_cpus().size(); }

    // Runs the child probe with only the first `count` allowed CPUs; true if it passed.
    bool probe_under_affinity( std::size_t const count )
    {
        auto const cpus{ allowed_cpus() };
        auto const self{ std::string( "/proc/self/exe" ) };
        auto const expected{ std::to_string( count ) };
        auto const pid{ ::fork() };
        if ( pid == 0 )
        {
            cpu_set_t subset;
            CPU_ZERO( &subset );
            for ( std::size_t i{ 0 }; i < count; ++i )
                CPU_SET( cpus[ i ], &subset );
            if ( ::sched_setaffinity( 0, sizeof( subset ), &subset ) != 0 )
                ::_exit( 90 );
            ::setenv( expected_variable, expected.c_str(), 1 );
            ::execl( self.c_str(), self.c_str(), "--gtest_filter=HardwareConcurrency.ChildProbe", static_cast<char *>( nullptr ) );
            ::_exit( 91 );
        }
        int status{ -1 };
        return
            ( ::waitpid( pid, &status, 0 ) == pid ) &&
            WIFEXITED( status ) &&
            ( WEXITSTATUS( status ) == 0 );
    }
#elif defined( _WIN32 )
    std::size_t available_cpu_count()
    {
        DWORD_PTR process_mask;
        DWORD_PTR system_mask;
        if ( !::GetProcessAffinityMask( ::GetCurrentProcess(), &process_mask, &system_mask ) )
            return 0;
        std::size_t result{ 0 };
        for ( DWORD_PTR bit{ 1 }; bit != 0; bit <<= 1 )
            if ( process_mask & bit )
                ++result;
        return result;
    }

    bool probe_under_affinity( std::size_t const count )
    {
        DWORD_PTR process_mask;
        DWORD_PTR system_mask;
        if ( !::GetProcessAffinityMask( ::GetCurrentProcess(), &process_mask, &system_mask ) )
            return false;
        DWORD_PTR subset{ 0 };
        std::size_t picked{ 0 };
        for ( DWORD_PTR bit{ 1 }; ( bit != 0 ) && ( picked < count ); bit <<= 1 )
            if ( process_mask & bit )
            {
                subset |= bit;
                ++picked;
            }
        if ( picked < count )
            return false;
        char path[ MAX_PATH ];
        ::GetModuleFileNameA( nullptr, path, MAX_PATH );
        ::SetEnvironmentVariableA( expected_variable, std::to_string( count ).c_str() );
        auto command{ std::string( "\"" ) + path + "\" --gtest_filter=HardwareConcurrency.ChildProbe" };
        STARTUPINFOA startup{ .cb = sizeof( startup ) };
        PROCESS_INFORMATION process{};
        auto const created{ ::CreateProcessA( path, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process ) };
        ::SetEnvironmentVariableA( expected_variable, nullptr );
        if ( !created )
            return false;
        auto const restricted{ ::SetProcessAffinityMask( process.hProcess, subset ) };
        ::ResumeThread( process.hThread );
        ::WaitForSingleObject( process.hProcess, INFINITE );
        DWORD exit_code{ 1 };
        ::GetExitCodeProcess( process.hProcess, &exit_code );
        ::CloseHandle( process.hThread );
        ::CloseHandle( process.hProcess );
        return
            restricted &&
            ( exit_code == 0 );
    }
#endif
} // anonymous namespace
//------------------------------------------------------------------------------

TEST( HardwareConcurrency, ChildProbe )
{
    auto const * const expected{ std::getenv( expected_variable ) };
    if ( !expected )
        GTEST_SKIP() << "only meaningful when launched by the affinity tests";
    EXPECT_EQ( psi::thrd_lite::get_hardware_concurrency_max(), std::atoi( expected ) );
    EXPECT_EQ( static_cast<int>( psi::thrd_lite::hardware_concurrency_max ), std::atoi( expected ) );
}

#if defined( __linux__ ) || defined( _WIN32 )
TEST( HardwareConcurrency, FollowsAffinityMask )
{
    if ( available_cpu_count() < 3 )
        GTEST_SKIP() << "needs at least 3 allowed CPUs to build a distinguishable mask";
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
