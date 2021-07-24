////////////////////////////////////////////////////////////////////////////////
///
/// \file thread.hpp
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
#pragma once
//------------------------------------------------------------------------------
#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#include <windows.h>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

enum struct priority : int
{
    idle          = THREAD_PRIORITY_LOWEST, // TODO THREAD_MODE_BACKGROUND_BEGIN
    background    = THREAD_PRIORITY_LOWEST,
    low           = THREAD_PRIORITY_BELOW_NORMAL,
    normal        = THREAD_PRIORITY_NORMAL,
    high          = THREAD_PRIORITY_ABOVE_NORMAL,
    foreground    = THREAD_PRIORITY_HIGHEST,
    time_critical = THREAD_PRIORITY_TIME_CRITICAL
}; // enum struct priority


class thread_impl
{
public:
    using native_handle_type = ::HANDLE;
    using id                 = ::DWORD ;

    BOOST_NOTHROW_LITE void join() noexcept
    {
        BOOST_VERIFY( ::WaitForSingleObjectEx( handle_, INFINITE, false ) == WAIT_OBJECT_0 );
    #ifndef NDEBUG
        DWORD exitCode;
        BOOST_VERIFY( ::GetExitCodeThread( handle_, &exitCode ) );
        BOOST_ASSERT( exitCode == 0 );
    #endif // NDEBUG
        detach();
    }

    BOOST_NOTHROW_LITE void detach() noexcept
    {
        BOOST_VERIFY( ::CloseHandle( handle_ ) );
        handle_ = {};
    }

    auto get_id    () const noexcept { return ::GetThreadId( handle_ ); }
    auto get_handle() const noexcept { return handle_; }

    static auto get_active_thread_id() noexcept { return ::GetCurrentThreadId(); }

    BOOST_ATTRIBUTES( BOOST_MINSIZE )
    bool set_priority( priority const new_priority ) noexcept
    {
        /// \note SetThreadPriority() silently falls back to the highest
        /// priority level available to the caller based on its privileges
        /// (instead of failing).
        ///                               (23.06.2017.) (Domagoj Saric)
        BOOST_VERIFY( ::SetThreadPriority( get_handle(), static_cast< int >( new_priority ) ) != false );
        return true;
    }

    class affinity_mask
    {
    public:
        void add_cpu( unsigned const cpu_id ) noexcept { value_ |= DWORD_PTR( 1 ) << cpu_id; }

    private: friend class thread_impl;
        DWORD_PTR value_ = 0;
    }; // class affinity_mask

    BOOST_ATTRIBUTES( BOOST_COLD )
    auto bind_to_cpu( affinity_mask const mask ) noexcept
    {
        return ::SetThreadAffinityMask( get_handle(), mask.value_ ) != 0;
    }

protected:
    thread_impl() = default;
   ~thread_impl() = default;

    using thread_procedure = PTHREAD_START_ROUTINE;

    BOOST_NOTHROW_LITE auto create( thread_procedure const start_routine, void * const arg ) noexcept
    {
        handle_ = ::CreateThread( nullptr, 0, start_routine, arg, 0, nullptr );
        if ( BOOST_UNLIKELY( handle_ == nullptr ) )
        {
            BOOST_ASSERT( ::GetLastError() == ERROR_NOT_ENOUGH_MEMORY ); // any other error indicates a programmer error
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        return ERROR_SUCCESS;
    }

protected:
    native_handle_type handle_{};
}; // class thread_impl

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
