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
#include <boost/config_ex.hpp>

#include <pthread.h>

#if defined( __linux )
#include <sys/time.h>
#include <sys/resource.h>
#   ifdef __GLIBC__
    // Glibc pre 2.3 does not provide the wrapper for the gettid system call
    // https://stackoverflow.com/a/36025103
    // https://linux.die.net/man/2/gettid
#   include <unistd.h>
#   include <sys/syscall.h>
#ifdef BOOST_GCC
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wattributes"
#endif // GCC
    __attribute__(( const, weak )) inline
    pid_t gettid() { return syscall( SYS_gettid ); }
#ifdef BOOST_GCC
#    pragma GCC diagnostic pop
#endif // GCC
#   endif // glibc
#endif // __linux
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace thrd_lite
{
//------------------------------------------------------------------------------

enum struct priority : int
{
    idle          =  19,
    background    =  10,
    low           =   5,
    normal        =   0,
    high          =  -5,
    foreground    = -10,
    time_critical = -20
}; // enum struct priority

class thread_impl
{
public:
    using native_handle_type = pthread_t;
    using id                 = pthread_t;

    class affinity_mask
    {
#ifdef __linux__
    public:
        affinity_mask() noexcept { CPU_ZERO( &value_ ); }

        void add_cpu( unsigned const cpu_id ) noexcept { CPU_SET( cpu_id, &value_ ); }

    private: friend class thread_impl;
        cpu_set_t value_;
#endif // __linux__
    }; // class affinity_mask

    void               join  ()       noexcept;
    void               detach()       noexcept;
    native_handle_type get_id() const noexcept;

    static auto get_active_thread_id() noexcept BOOST_NOTHROW_LITE { return pthread_self(); }

    bool set_priority( priority      ) noexcept;
    bool bind_to_cpu ( affinity_mask ) noexcept;

    static bool bind_to_cpu( native_handle_type, affinity_mask ) noexcept;

protected:
    // https://stackoverflow.com/questions/43819314/default-member-initializer-needed-within-definition-of-enclosing-class-outside
    constexpr thread_impl() noexcept {};
             ~thread_impl() noexcept = default;

    using thread_procedure = void * (*)( void * );

    int create( thread_procedure start_routine, void * arg ) noexcept BOOST_NOTHROW_LITE;

protected:
    native_handle_type handle_{};
}; // class thread_impl

//------------------------------------------------------------------------------
} // namespace thrd_lite
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
