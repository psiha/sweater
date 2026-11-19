////////////////////////////////////////////////////////////////////////////////
//
// Re-launches the running test executable restricted to a subset of the CPUs
// it is allowed to run on.
//
// The pool size (get_hardware_concurrency_max()) is computed once per process,
// so a restricted affinity mask can only be observed by a process that starts
// with it: a test that needs one runs its cases in a child of its own
// executable, started with the mask applied and a gtest filter selecting them.
//
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef __linux__
#   include <sched.h>
#   include <signal.h>
#   include <sys/wait.h>
#   include <time.h>
#   include <unistd.h>
#elif defined( _WIN32 )
#   include <windows.h>
#endif
//------------------------------------------------------------------------------
namespace psi::sweater::test
{
//------------------------------------------------------------------------------

// Set (to the number of CPUs it was given) in a child started by
// run_under_affinity().
char constexpr restricted_cpus_variable[]{ "PSI_SWEATER_TEST_RESTRICTED_CPUS" };

// The number of CPUs this process was restricted to by run_under_affinity(),
// 0 for a process that was not started by it.
inline int restricted_cpus()
{
    auto const * const value{ std::getenv( restricted_cpus_variable ) };
    return value ? std::atoi( value ) : 0;
}

// A child that has not finished by then is killed and counts as failed: a
// pool that cannot serve its work under the mask shows as a hang.
auto constexpr child_deadline_seconds{ 120 };

#ifdef __linux__
inline std::vector<int> allowed_cpus()
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

inline std::size_t available_cpu_count() { return allowed_cpus().size(); }

// Runs the tests matching `filter` in a child restricted to the first `count`
// allowed CPUs; true if the child ran to the end and every test passed.
inline bool run_under_affinity( std::size_t const count, std::string const & filter )
{
    auto const cpus{ allowed_cpus() };
    if ( cpus.size() < count )
        return false;
    auto const self    { std::string( "/proc/self/exe" ) };
    auto const argument{ "--gtest_filter=" + filter };
    auto const value   { std::to_string( count ) };
    auto const pid{ ::fork() };
    if ( pid == 0 )
    {
        cpu_set_t subset;
        CPU_ZERO( &subset );
        for ( std::size_t i{ 0 }; i < count; ++i )
            CPU_SET( cpus[ i ], &subset );
        if ( ::sched_setaffinity( 0, sizeof( subset ), &subset ) != 0 )
            ::_exit( 90 );
        ::setenv( restricted_cpus_variable, value.c_str(), 1 );
        ::execl( self.c_str(), self.c_str(), argument.c_str(), static_cast<char *>( nullptr ) );
        ::_exit( 91 );
    }
    if ( pid < 0 )
        return false;
    int status{ -1 };
    for ( auto waited_ms{ 0 }; ; waited_ms += 10 )
    {
        auto const result{ ::waitpid( pid, &status, WNOHANG ) };
        if ( result == pid )
            break;
        if ( ( result < 0 ) || ( waited_ms >= child_deadline_seconds * 1000 ) )
        {
            ::kill( pid, SIGKILL );
            ::waitpid( pid, &status, 0 );
            return false;
        }
        timespec const pause{ .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        ::nanosleep( &pause, nullptr );
    }
    return
        WIFEXITED( status ) &&
        ( WEXITSTATUS( status ) == 0 );
}
#elif defined( _WIN32 )
inline std::size_t available_cpu_count()
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

inline bool run_under_affinity( std::size_t const count, std::string const & filter )
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
    ::SetEnvironmentVariableA( restricted_cpus_variable, std::to_string( count ).c_str() );
    auto command{ std::string( "\"" ) + path + "\" --gtest_filter=" + filter };
    STARTUPINFOA startup{ .cb = sizeof( startup ) };
    PROCESS_INFORMATION process{};
    auto const created{ ::CreateProcessA( path, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process ) };
    ::SetEnvironmentVariableA( restricted_cpus_variable, nullptr );
    if ( !created )
        return false;
    auto const restricted{ ::SetProcessAffinityMask( process.hProcess, subset ) };
    ::ResumeThread( process.hThread );
    DWORD exit_code{ 1 };
    if ( ::WaitForSingleObject( process.hProcess, child_deadline_seconds * 1000 ) == WAIT_OBJECT_0 )
        ::GetExitCodeProcess( process.hProcess, &exit_code );
    else
        ::TerminateProcess( process.hProcess, 1 );
    ::CloseHandle( process.hThread );
    ::CloseHandle( process.hProcess );
    return
        restricted &&
        ( exit_code == 0 );
}
#endif

//------------------------------------------------------------------------------
} // namespace psi::sweater::test
//------------------------------------------------------------------------------
