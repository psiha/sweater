////////////////////////////////////////////////////////////////////////////////
///
/// \file thread.cpp
/// ----------------
///
/// (c) Copyright Domagoj Saric 2016 - 2021.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "thread.hpp"

#include <boost/assert.hpp>

#include <errno.h>
#ifdef __ANDROID__
#include <sys/time.h>
#include <sys/resource.h>
#endif // __ANDROID__
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

namespace
{
#if !defined( __ANDROID__ )
#ifdef __EMSCRIPTEN__
    auto const default_policy_priority_min        { 0 };
    auto const default_policy_priority_max        { 0 };
#else
    auto const default_policy_priority_min        { ::sched_get_priority_min( SCHED_OTHER ) };
    auto const default_policy_priority_max        { ::sched_get_priority_max( SCHED_OTHER ) };
#endif
    auto const default_policy_priority_range      { static_cast<std::uint8_t>( default_policy_priority_max - default_policy_priority_min ) };
    auto const default_policy_priority_unchangable{ default_policy_priority_range == 0 };

    std::uint8_t round_divide( std::uint16_t const numerator, std::uint8_t const denominator ) noexcept
    {
        auto const integral_division      {   numerator / denominator                          };
        auto const at_least_half_remainder{ ( numerator % denominator ) >= ( denominator / 2 ) };
        return integral_division + at_least_half_remainder;
    }
#endif // !__ANDROID__
} // anonymous namespace

void                            BOOST_NOTHROW_LITE thread_impl::join  ()       noexcept { BOOST_VERIFY( pthread_join  ( handle_, nullptr ) == 0 ); handle_ = {}; }
void                            BOOST_NOTHROW_LITE thread_impl::detach()       noexcept { BOOST_VERIFY( pthread_detach( handle_          ) == 0 ); handle_ = {}; }
thread_impl::native_handle_type BOOST_NOTHROW_LITE thread_impl::get_id() const noexcept { return handle_; }

BOOST_ATTRIBUTES( BOOST_MINSIZE )
bool thread_impl::set_priority( priority const new_priority ) noexcept
{
#ifdef __EMSCRIPTEN__
    if constexpr ( true )
        return ( new_priority == priority::normal );
#endif
    auto const nice_value{ static_cast<int>( new_priority ) };
#if defined( __ANDROID__ )
    /// \note Android's pthread_setschedparam() does not actually work so we
    /// have to abuse the general Linux' setpriority() non-POSIX compliance
    /// (i.e. that it sets the calling thread's priority).
    /// http://stackoverflow.com/questions/17398075/change-native-thread-priority-on-android-in-c-c
    /// https://android.googlesource.com/platform/dalvik/+/gingerbread/vm/alloc/Heap.c
    /// https://developer.android.com/topic/performance/threads.html
    /// _or_
    /// try the undocumented things the Java Process.setThreadPriority()
    /// function seems to be doing:
    /// https://github.com/android/platform_frameworks_base/blob/master/core/java/android/os/Process.java#L634
    /// https://github.com/android/platform_frameworks_base/blob/master/core/jni/android_util_Process.cpp#L475
    /// https://android.googlesource.com/platform/frameworks/native/+/jb-dev/libs/utils/Threads.cpp#329
    ///                                   (03.05.2017.) (Domagoj Saric)
    return ::setpriority( PRIO_PROCESS, get_id(), nice_value ) == 0;
#else
    std::uint8_t const api_range            { static_cast<std::int8_t>( priority::idle ) - static_cast<std::int8_t>( priority::time_critical ) };
    auto         const platform_range       { detail::default_policy_priority_range };
    auto         const uninverted_nice_value{ static_cast<std::uint8_t>( - ( nice_value - static_cast<std::int8_t>( priority::idle ) ) ) };
    int          const priority_value       { detail::default_policy_priority_min + detail::round_divide( uninverted_nice_value * platform_range, api_range ) }; // surely it will be hoisted
#   if defined( __APPLE__ )
    BOOST_ASSERT( !detail::default_policy_priority_unchangable );
    ::sched_param scheduling_parameters;
    int           policy;
    auto const handle{ thread.native_handle() };
    BOOST_VERIFY( pthread_getschedparam( handle, &policy, &scheduling_parameters ) == 0 );
    scheduling_parameters.sched_priority = priority_value;
    return pthread_setschedparam( handle, policy, &scheduling_parameters ) == 0;
#   else
    return !detail::default_policy_priority_unchangable && ( pthread_setschedprio( thread.get_id(), priority_value ) == 0 );
#   endif // Apple or
#endif // Android or
}


bool thread_impl::bind_to_cpu( [[ maybe_unused ]] affinity_mask const mask ) noexcept
{
    // Android does not have pthread_setaffinity_np or pthread_attr_setaffinity_np
    // and there seems to be no way of detecting its presence.
#if 0
    return pthread_setaffinity_np( get_id(), sizeof( mask.value_ ), &mask.value_ ) == 0;
#elif 0
    return bind_to_cpu( pthread_getthreadid_np( get_id() ), mask ) == 0;
#else
    return false;
#endif
}

BOOST_ATTRIBUTES( BOOST_COLD )
bool thread_impl::bind_to_cpu( native_handle_type const handle, affinity_mask const mask ) noexcept
{
    return sched_setaffinity( handle, sizeof( mask.value_ ), &mask.value_ ) == 0;
}

int BOOST_NOTHROW_LITE thread_impl::create( thread_procedure const start_routine, void * const arg ) noexcept
{
    auto const error{ pthread_create( &handle_, nullptr, start_routine, arg ) };
    if ( BOOST_UNLIKELY( error ) )
    {
        BOOST_ASSUME( error == EAGAIN ); // any other error indicates a programmer error
        handle_ = {};
    }
    return error;
}

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
