////////////////////////////////////////////////////////////////////////////////
///
/// \file generic.hpp
/// -----------------
///
/// (c) Copyright Domagoj Saric 2016 - 2020.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#ifndef generic_hpp__99FE2034_248F_4C7D_8CD2_EB2BB8247377
#define generic_hpp__99FE2034_248F_4C7D_8CD2_EB2BB8247377
#pragma once
//------------------------------------------------------------------------------
#include "../hardware_concurrency.hpp"
#include "../queues/mpmc_moodycamel.hpp"

#include <boost/core/no_exceptions_support.hpp>
#include <boost/config_ex.hpp>
#include <boost/container/static_vector.hpp>
#include <boost/functionoid/functionoid.hpp>
#include <boost/range/iterator_range_core.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iterator>
#ifdef BOOST_MSVC
#include <malloc.h>
#else
#include <alloca.h>
#endif // BOOST_MSVC
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#if 0 // sacrifice standard conformance to avoid the overhead of system_error
#include <system_error>
#endif // disabled

#ifdef BOOST_HAS_PTHREADS
#include <pthread.h>
#else
#include <windows.h> // for SetThreadPriority
#endif // BOOST_HAS_PTHREADS

#if defined( __linux )
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>

__attribute__(( const, weak ))
pid_t gettid() { return syscall( SYS_gettid ); }
#endif // __linux
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------
namespace queues { template <typename Work> class mpmc_moodycamel; }
//------------------------------------------------------------------------------
namespace generic
{
//------------------------------------------------------------------------------

#if defined( BOOST_HAS_PTHREADS ) && !defined( __ANDROID__ )
namespace detail
{
#ifdef __EMSCRIPTEN__
    inline auto const default_policy_priority_min        ( 0 );
    inline auto const default_policy_priority_max        ( 0 );
#else
    inline auto const default_policy_priority_min        ( ::sched_get_priority_min( SCHED_OTHER ) );
    inline auto const default_policy_priority_max        ( ::sched_get_priority_max( SCHED_OTHER ) );
#endif
    inline auto const default_policy_priority_range      ( static_cast<std::uint8_t>( default_policy_priority_max - default_policy_priority_min ) );
    inline auto const default_policy_priority_unchangable( default_policy_priority_range == 0 );

    inline
    std::uint8_t round_divide( std::uint16_t const numerator, std::uint8_t const denominator ) noexcept
    {
        auto const integral_division      (   numerator / denominator                          );
        auto const at_least_half_remainder( ( numerator % denominator ) >= ( denominator / 2 ) );
        return integral_division + at_least_half_remainder;
    }
} // namespace detail
#endif // __linux && !__ANDROID__ || __APPLE__

// Resource saving option if jobs are always dispatched from a single thread
// (i.e. there is no fear of overcommiting the CPU)
#ifndef BOOST_SWEATER_USE_CALLER_THREAD
#   define BOOST_SWEATER_USE_CALLER_THREAD false
#endif

class shop
{
public:
    using iterations_t = std::uint32_t;

    enum struct priority : int
    {
    #ifdef BOOST_HAS_PTHREADS
        idle          =  19,
        background    =  10,
        low           =  5,
        normal        =  0,
        high          = -5,
        foreground    = -10,
        time_critical = -20
    #else
        idle          = THREAD_PRIORITY_LOWEST, // TODO THREAD_MODE_BACKGROUND_BEGIN
        background    = THREAD_PRIORITY_LOWEST,
        low           = THREAD_PRIORITY_BELOW_NORMAL,
        normal        = THREAD_PRIORITY_NORMAL,
        high          = THREAD_PRIORITY_ABOVE_NORMAL,
        foreground    = THREAD_PRIORITY_HIGHEST,
        time_critical = THREAD_PRIORITY_TIME_CRITICAL
    #endif // thread backend
    };

private:
#ifdef BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
    // https://petewarden.com/2015/10/11/one-weird-trick-for-faster-android-multithreading
    static std::uint32_t spin_count;
#endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION

    struct worker_traits : functionoid::std_traits
    {
        static constexpr auto copyable    = functionoid::support_level::na     ;
        static constexpr auto moveable    = functionoid::support_level::nofail ;
        static constexpr auto destructor  = functionoid::support_level::trivial;
        static constexpr auto is_noexcept = true;
        static constexpr auto rtti        = false;

#   ifdef __EMSCRIPTEN__
        // https://github.com/emscripten-core/emscripten/issues/5996
        static constexpr std::uint8_t sbo_alignment = 8;
#   else
        static constexpr std::uint8_t sbo_alignment = 16;
#   endif

        using empty_handler = functionoid::assert_on_empty;
    }; // struct worker_traits

    using worker_counter = std::atomic<hardware_concurrency_t>;

    //...mrmlj...lighterweight alternatives to std::thread...to be moved to a separate lib...
#ifdef BOOST_HAS_PTHREADS
    class pthread_condition_variable;
    class pthread_mutex
    {
    public:
        pthread_mutex() noexcept BOOST_NOTHROW_LITE
#       if !defined( NDEBUG ) && defined( PTHREAD_ERRORCHECK_MUTEX_INITIALIZER )
            : mutex_( PTHREAD_ERRORCHECK_MUTEX_INITIALIZER ) {}
#       else
            : mutex_( PTHREAD_MUTEX_INITIALIZER ) {}
#       endif // NDEBUG
        ~pthread_mutex() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_mutex_destroy( &mutex_ ) == 0 ); }

        pthread_mutex( pthread_mutex && other ) noexcept : mutex_( other.mutex_ ) { other.mutex_ = PTHREAD_MUTEX_INITIALIZER; }
        pthread_mutex( pthread_mutex const & ) = delete;

        void   lock() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_mutex_lock  ( &mutex_ ) == 0 ); }
        void unlock() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_mutex_unlock( &mutex_ ) == 0 ); }

        bool try_lock() noexcept BOOST_NOTHROW_LITE { return pthread_mutex_trylock( &mutex_ ) == 0; }

    private: friend class pthread_condition_variable;
        pthread_mutex_t mutex_;
    }; // class pthread_mutex

    class pthread_condition_variable
    {
    public:
        pthread_condition_variable() noexcept BOOST_NOTHROW_LITE : cv_( PTHREAD_COND_INITIALIZER ) {}
       ~pthread_condition_variable() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_cond_destroy( &cv_ ) == 0 ); }

        pthread_condition_variable( pthread_condition_variable && other ) noexcept : cv_( other.cv_ ) { other.cv_ = PTHREAD_COND_INITIALIZER; }
        pthread_condition_variable( pthread_condition_variable const & ) = delete;

        void notify_all() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_cond_broadcast( &cv_ ) == 0 ); }
        void notify_one() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_cond_signal   ( &cv_ ) == 0 ); }

        void wait( std::unique_lock<pthread_mutex> & lock   ) noexcept BOOST_NOTHROW_LITE { wait( *lock.mutex() ); }
        void wait( pthread_mutex & mutex /*must be locked*/ ) noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_cond_wait( &cv_, &mutex.mutex_ ) == 0 ); }

    private:
        pthread_cond_t cv_;
    }; // class pthread_condition_variable

    class thread_impl
    {
    public:
        using native_handle_type = pthread_t;
        using id                 = pthread_t;

        void join  () noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_join  ( handle_, nullptr ) == 0 ); handle_ = {}; }
        void detach() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( pthread_detach( handle_          ) == 0 ); handle_ = {}; }
        auto get_id() const noexcept { return handle_; }

        static auto get_active_thread_id() noexcept BOOST_NOTHROW_LITE { return pthread_self(); }

    protected:
       // https://stackoverflow.com/questions/43819314/default-member-initializer-needed-within-definition-of-enclosing-class-outside
       constexpr  thread_impl() noexcept {};
                 ~thread_impl() noexcept = default;

        using thread_procedure = void * (*) ( void * );

        auto create( thread_procedure const start_routine, void * const arg ) noexcept BOOST_NOTHROW_LITE
        {
            auto const error( pthread_create( &handle_, nullptr, start_routine, arg ) );
            if ( BOOST_UNLIKELY( error ) )
            {
            #ifndef __ANDROID__
                /// \note NDK 15c Clang miscompiles this function to
                /// allways assume that error == EAGAIN (even if it is 0) if the
                /// line below is included.
                ///                           (29.09.2017.) (Domagoj Saric)
                BOOST_ASSUME( error == EAGAIN ); // any other error indicates a programmer error
            #endif // !__ANDROID__
                handle_ = {};
            }
            return error;
        }

    protected:
        native_handle_type handle_{};
    }; // class thread_impl

    using condition_variable = pthread_condition_variable;
    using mutex              = pthread_mutex;
#else // Win32
    // Strategies for Implementing POSIX Condition Variables on Win32 http://www.cs.wustl.edu/~schmidt/win32-cv-1.html
    // http://developers.slashdot.org/story/07/02/26/1211220/pthreads-vs-win32-threads
    // http://nasutechtips.blogspot.com/2010/11/slim-read-write-srw-locks.html

    class condition_variable;
    class mutex
    {
    public:
        mutex(                ) noexcept : lock_{ SRWLOCK_INIT } {}
        mutex( mutex && other ) noexcept : lock_( other.lock_ ) { other.lock_ = { SRWLOCK_INIT }; }
        mutex( mutex const &  ) = delete ;
       ~mutex(                ) = default;

        void   lock() noexcept { ::AcquireSRWLockExclusive( &lock_ ); }
        void unlock() noexcept { ::ReleaseSRWLockExclusive( &lock_ ); }

        bool try_lock() noexcept { return ::TryAcquireSRWLockExclusive( &lock_ ) != false; }

    private: friend class condition_variable;
        ::SRWLOCK lock_;
    }; // class mutex

    class condition_variable
    {
    public:
        using lock_t = std::unique_lock<mutex>;

        condition_variable(                             ) noexcept : cv_{ CONDITION_VARIABLE_INIT } {}
        condition_variable( condition_variable && other ) noexcept : cv_( other.cv_ ) { other.cv_ = { CONDITION_VARIABLE_INIT }; }
        condition_variable( condition_variable const &  ) = delete ;
       ~condition_variable(                             ) = default;

        void notify_all() noexcept { ::WakeAllConditionVariable( &cv_ ); }
        void notify_one() noexcept { ::WakeConditionVariable   ( &cv_ ); }

        void wait( lock_t & lock ) noexcept { BOOST_VERIFY( wait( lock, INFINITE ) ); }
        void wait( mutex  & m    ) noexcept { BOOST_VERIFY( wait( m   , INFINITE ) ); }

        bool wait( lock_t & lock                , std::uint32_t const milliseconds ) noexcept { return wait( *lock.mutex(), milliseconds ); }
        bool wait( mutex  & m /*must be locked*/, std::uint32_t const milliseconds ) noexcept
        {
            auto const result( ::SleepConditionVariableSRW( &cv_, &m.lock_, milliseconds, 0/*CONDITION_VARIABLE_LOCKMODE_SHARED*/ ) );
            BOOST_ASSERT( result || ::GetLastError() == ERROR_TIMEOUT );
            return result != false;
        }

    private:
        ::CONDITION_VARIABLE cv_;
    }; // class condition_variable

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
#endif // BOOST_HAS_PTHREADS

    class thread : public thread_impl
    {
    private:
        class synchronized_invocation
        {
        public:
            synchronized_invocation( void const * const p_functor ) noexcept
                :
                p_functor_     ( const_cast<void *>( p_functor ) ),
                lock_          ( mutex_                          ),
                functor_copied_( false                           )
            {}

            ~synchronized_invocation() noexcept
            {
                while ( BOOST_UNLIKELY( !functor_copied_ ) )
                    event_.wait( lock_ );
            }

            template <typename Functor>
            auto & functor() const noexcept { return *static_cast<Functor *>( p_functor_ ); }

            auto notify() noexcept
            {
                functor_copied_ = true;
                event_.notify_one();
            }

        private:
            void *                  const p_functor_;
            mutex                         mutex_;
            condition_variable            event_;
            std::unique_lock<mutex>       lock_;
            bool volatile                 functor_copied_;
        }; // struct synchronized_invocation

    public:
        thread() noexcept = default;
       ~thread() noexcept { BOOST_ASSERT_MSG( !joinable(), "Abandoning a thread!" ); }

        thread( thread && other ) noexcept { swap( other ); }
        thread( thread const & ) = delete;

        thread & operator=( thread && other ) noexcept
        {
            this->handle_ = other.handle_;
            other.handle_ = {};
            return *this;
        }

        template <class F>
        thread & operator=( F && functor )
        {
            using ret_t   = std::invoke_result_t<thread_procedure, void *>;
            using Functor = std::decay_t<F>;

            if constexpr
            (
                ( sizeof ( functor ) <= sizeof ( void * ) ) &&
                ( alignof( Functor ) <= alignof( void * ) ) &&
                std::is_trivially_copy_constructible_v<Functor> &&
                std::is_trivially_destructible_v      <Functor>
            )
            {
                void * context;
                new ( &context ) Functor( std::forward<F>( functor ) );
                create
                (
                    []( void * context ) noexcept -> ret_t
                    {
                        auto & tiny_functor( reinterpret_cast<Functor &>( context ) );
                        tiny_functor();
                        return 0;
                    },
                    context
                );
            }
            else
            if constexpr ( noexcept( Functor( std::forward<F>( functor ) ) ) )
            {
                synchronized_invocation context( &functor );

                create
                (
                    []( void * const context ) noexcept -> ret_t
                    {
                        auto & synchronized_context( *static_cast<synchronized_invocation *>( context ) );
                        Functor functor( std::forward<F>( synchronized_context.functor<Functor>() ) );
                        synchronized_context.notify();
                        functor();
                        return 0;
                    },
                    &context
                );
            }
            else
            {
                auto p_functor( std::make_unique<Functor>( std::forward<F>( functor ) ) );
                create
                (
                    []( void * const context ) noexcept -> ret_t
                    {
                        std::unique_ptr<Functor> const p_functor( static_cast<Functor *>( context ) );
                        (*p_functor)();
                        return 0;
                    },
                    p_functor.get()
                );
                p_functor.release();
            }
            return *this;
        }

        auto native_handle() const noexcept { return handle_; }

        bool joinable() const noexcept { return native_handle() != native_handle_type{}; }

        void join() noexcept
        {
            BOOST_ASSERT_MSG( joinable(), "No thread to join" );
            BOOST_ASSERT_MSG( get_id() != get_active_thread_id(), "Waiting on this_thread: deadlock!" );

            thread_impl::join();
        }

        void swap( thread & other ) noexcept { std::swap( this->handle_, other.handle_ ); }

        static auto hardware_concurrency() noexcept { return hardware_concurrency_max; }

    private:
        BOOST_ATTRIBUTES( BOOST_COLD )
        void create( thread_procedure const start_routine, void * const arg )
        {
            BOOST_ASSERT_MSG( !joinable(), "A thread already created" );
            auto const error( thread_impl::create( start_routine, arg ) );
            if ( BOOST_UNLIKELY( error ) )
            {
#           ifdef BOOST_NO_EXCEPTIONS
                std::terminate();
#           elif 0 // disabled - avoid the overhead of (at least) <system_error>
                throw std::system_error( std::error_code( error, std::system_category() ), "Thread creation failed" );
#           else
                throw std::runtime_error( "Not enough resources to create a new thread" );
#           endif
            }
        }
    }; // class thread

    class barrier
    {
    public:
        barrier( hardware_concurrency_t const initial_value ) noexcept
            :
            counter_( initial_value )
        {
            BOOST_VERIFY( mutex_.try_lock() );
        }
#   ifndef _WIN32 // not needed for trivial SRWs
        ~barrier() noexcept { mutex_.unlock(); }
#   endif

        BOOST_NOINLINE
        void arrive() noexcept
        {
            std::unique_lock<mutex> lock( mutex_ );
            if ( counter_.fetch_sub( 1, std::memory_order_relaxed ) == 1 )
                event_.notify_one();
        }

        BOOST_NOINLINE
        void wait() noexcept
        {
            while ( BOOST_UNLIKELY( counter_.load( std::memory_order_relaxed ) != 0 ) )
                event_.wait( mutex_ );
        }

        void mark_as_arrived_to_by_all() noexcept { counter_.store( 0, std::memory_order_relaxed ); }

    private:
        // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2007/n2406.html#gen_cond_var
        mutex              mutex_  ;
        condition_variable event_  ;
        worker_counter     counter_;
    }; // class barrier

    class spread_setup
    {
    public:
        spread_setup( iterations_t const iterations, hardware_concurrency_t const actual_number_of_workers ) noexcept
            :
            iterations_per_worker          ( iterations / actual_number_of_workers ),
            threads_with_extra_iteration   ( iterations % actual_number_of_workers - leave_one_for_the_calling_thread() ),
            number_of_dispatched_work_parts( number_of_work_parts( iterations, actual_number_of_workers ) - BOOST_SWEATER_USE_CALLER_THREAD ),
            completion_barrier             ( number_of_dispatched_work_parts )
        {
            BOOST_ASSERT( ( leave_one_for_the_calling_thread() == false ) || ( iterations < iterations_t( actual_number_of_workers ) ) );
#       if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
            BOOST_ASSUME( number_of_dispatched_work_parts <= ( BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY - BOOST_SWEATER_USE_CALLER_THREAD ) );
#       endif
        }

        iterations_t           const iterations_per_worker;
        hardware_concurrency_t const threads_with_extra_iteration;
        hardware_concurrency_t const number_of_dispatched_work_parts;
        barrier                      completion_barrier;

    private:
        // If iterations < workers prefer using the caller thread instead of waking up a worker thread...
        bool leave_one_for_the_calling_thread() const noexcept { return ( iterations_per_worker == 0 ) && BOOST_SWEATER_USE_CALLER_THREAD; }
        static hardware_concurrency_t number_of_work_parts( iterations_t const iterations, hardware_concurrency_t const actual_number_of_workers ) noexcept
        {
            return static_cast<hardware_concurrency_t>( std::min<iterations_t>( actual_number_of_workers, iterations ) );
        }
    }; // class spread_setup

    using work_t = functionoid::callable<void(), worker_traits>;

    using my_queue = queues::mpmc_moodycamel<work_t>;

    auto worker_loop( [[ maybe_unused ]] hardware_concurrency_t const worker_index ) noexcept
    {
        auto worker_loop_impl
        {
            [=, this]() noexcept
            {
#           if BOOST_SWEATER_EXACT_WORKER_SELECTION
                auto       & __restrict work_event{ pool_[ worker_index ].event };
#           else
                auto       & __restrict work_event{ work_event_ };
#           endif
                auto       & __restrict queue     { queue_      };
                auto       & __restrict work_mutex{ mutex_      };
                auto const & __restrict exit      { brexit_     };

                auto token( queue.consumer_token() );

                work_t work;

                for ( ; ; )
                {
#               ifdef BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
                    auto const dequeue_cost( 2048 );
                    for ( auto try_count( 0U ); try_count < (spin_count / dequeue_cost); ++try_count )
                    {
                        if ( BOOST_LIKELY( queue.dequeue( work, token ) ) )
                        {
                            work();
                            try_count = 0; // restart the spin-wait
                        }
                    }
#               endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION

                    bool have_work;
                    {
                        std::unique_lock<mutex> lock( work_mutex );
                        if ( BOOST_UNLIKELY( exit.load( std::memory_order_relaxed ) ) )
                            return;
                        /// \note No need for another loop here as a
                        /// spurious-wakeup would be handled by the check in
                        /// the loop above.
                        ///               (08.11.2016.) (Domagoj Saric)
                        have_work = queue.dequeue( work, token );
                        if ( BOOST_UNLIKELY( !have_work ) )
                            work_event.wait( lock );
                    }
                    if ( BOOST_LIKELY( have_work ) )
                        work();
                }
            }
        }; // worker_loop_impl
        static_assert( std::is_trivially_copy_constructible_v< decltype( worker_loop_impl ) > && std::is_trivially_destructible_v< decltype( worker_loop_impl ) > );
        return worker_loop_impl;
    }

public:
    shop()
    {
#   ifdef __GNUC__ // compilers with init_priority attribute (see hardware_concurency.hpp)
        auto const local_hardware_concurrency( hardware_concurrency_max );
#   else
        /// \note Avoid the static-initialization-order-fiasco (for compilers
        /// not supporting the init_priority attribute) by not using the
        /// global hardware_concurrency_max variable (i.e. allow users to
        /// safely create plain global-variable sweat_shop singletons).
        ///                                   (01.05.2017.) (Domagoj Saric)
        auto const local_hardware_concurrency( detail::get_hardware_concurrency_max() );
#   endif // __GNUC__
        create_pool( local_hardware_concurrency );
    }

    ~shop() noexcept { stop_and_destroy_pool(); }

    auto number_of_workers() const noexcept
    {
        auto const actual_number_of_workers( static_cast< hardware_concurrency_t >( pool_.size() ) );
#   if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        BOOST_ASSUME( actual_number_of_workers <= BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY );
#   endif
        return actual_number_of_workers;
    }

    /// For GCD dispatch_apply/OMP-like parallel loops.
    /// \details Guarantees that <VAR>work</VAR> will not be called more than
    /// <VAR>iterations</VAR> times (even if number_of_workers() > iterations).
    template <typename F>
    bool spread_the_sweat( iterations_t const iterations, F && __restrict work ) noexcept
    {
        static_assert( noexcept( work( iterations, iterations ) ), "F must be noexcept" );

#   if 0 // worth it...not worth it...
        if ( BOOST_UNLIKELY( iterations == 0 ) )
            return true;
#   endif
#   if 0 // worth it...not worth it...
        if ( BOOST_UNLIKELY( iterations == 1 ) )
        {
            work( 0, 1 );
            return true;
        }
#   endif

        spread_setup setup( iterations, number_of_workers() );

        /// \note MSVC does not support VLAs but has an alloca that returns (16
        /// byte) aligned memory. Clang's alloca is unaligned and it does not
        /// support VLAs of non-POD types. GCC has a (16 byte) aligned alloca
        /// and supports VLAs of non-POD types
        /// (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=19131).
        /// Regardless of any of this a work_t VLA is not used to avoid needless
        /// default construction of its members.
        /// The code below is safe with noexcept enqueue and noexcept
        /// destructible and constructible work_ts.
        ///                                   (21.01.2017.) (Domagoj Saric)
#   ifdef BOOST_MSVC
        auto const dispatched_work_parts{ static_cast<work_t *>( alloca( setup.number_of_dispatched_work_parts * sizeof( work_t ) ) ) };
#   else
        alignas( work_t ) char dispatched_work_parts_storage[ setup.number_of_dispatched_work_parts * sizeof( work_t ) ];
        auto * const BOOST_MAY_ALIAS dispatched_work_parts{ reinterpret_cast<work_t *>( dispatched_work_parts_storage ) };
#   endif // BOOST_MSVC

        iterations_t iteration{ 0 };
        for ( hardware_concurrency_t work_part{ 0 }; work_part < setup.number_of_dispatched_work_parts; ++work_part )
        {
            auto const start_iteration{ iteration };
            auto const extra_iteration{ work_part < setup.threads_with_extra_iteration };
            auto const end_iteration  { static_cast<iterations_t>( start_iteration + setup.iterations_per_worker + extra_iteration ) };
            auto const placeholder{ &dispatched_work_parts[ work_part ] };
#       ifdef BOOST_MSVC
            // MSVC14.1 still generates a branch w/o this (GCC issues a warning that it knows that &placeholder cannot be null so this has to be ifdef-guarded).
            BOOST_ASSUME( placeholder );
#       endif // BOOST_MSVC
            new ( placeholder ) work_t
            (
                [&completion_barrier = setup.completion_barrier, &work, start_iteration = iteration, end_iteration]() noexcept
                {
                    work( start_iteration, end_iteration );
                    completion_barrier.arrive();
                }
            );
            iteration = end_iteration;
        }

        auto const enqueue_failure_iteration_mask
        {
            enqueue( dispatched_work_parts, setup.number_of_dispatched_work_parts, setup.completion_barrier )
        };
        iteration &= enqueue_failure_iteration_mask;

        auto const caller_thread_start_iteration{ iteration };
#   if BOOST_SWEATER_USE_CALLER_THREAD
        BOOST_ASSERT( caller_thread_start_iteration < iterations );
        work( caller_thread_start_iteration, iterations );
#   else
        BOOST_VERIFY( caller_thread_start_iteration == iterations );
#   endif
        setup.completion_barrier.wait();

        return enqueue_failure_iteration_mask != 0;
    }

    template <typename F>
    bool fire_and_forget( F && work ) noexcept( noexcept( std::is_nothrow_constructible_v<std::remove_reference_t<F>, F &&> ) )
    {
        using Functor = std::remove_reference_t<F>;
        return create_fire_and_destroy<Functor>( std::forward<F>( work ) );
    }

    template <typename F>
    auto dispatch( F && work )
    {
        // http://scottmeyers.blogspot.hr/2013/03/stdfutures-from-stdasync-arent-special.html
        using Functor = std::remove_reference_t<F>;
        struct future_wrapper
        {
            // Note: Clang v8 and clang v9 think that result_t is unused
        #ifdef __clang__
        #   pragma clang diagnostic push
        #   pragma clang diagnostic ignored "-Wunused-local-typedef"
        #endif
            using result_t  = decltype( std::declval<Functor &>()() );
        #ifdef __clang__
        #   pragma clang diagnostic pop
        #endif
            using promise_t = std::promise<result_t>;
            using future_t  = std::future <result_t>;

            future_wrapper( F && work_source, future_t & future )
                :
                work( std::forward<F>( work_source ) )
            {
                future = promise.get_future();
            }

            void operator()() noexcept
            {
                BOOST_TRY
                {
                    if constexpr ( std::is_same_v<result_t, void> )
                    {
                        work();
                        promise.set_value();
                    }
                    else
                    {
                        promise.set_value( work() );
                    }
                }
                BOOST_CATCH( ... )
                {
                    promise.set_exception( std::current_exception() );
                }
                BOOST_CATCH_END
            }

            Functor   work   ;
            promise_t promise;
        }; // struct future_wrapper

        typename future_wrapper::future_t future;
        auto const dispatch_succeeded( this->create_fire_and_destroy<future_wrapper>( std::forward<F>( work ), future ) );
        if ( BOOST_UNLIKELY( !dispatch_succeeded ) )
        {
            typename future_wrapper::promise_t failed_promise;
            failed_promise.set_exception( std::make_exception_ptr( std::bad_alloc() ) );
            future = failed_promise.get_future();
        }
        return future;
    }

#ifdef BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
    static void set_idle_suspend_spin_count( std::uint32_t const new_spin_count ) noexcept
    {
        spin_count = new_spin_count;
    }
#endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION

    BOOST_ATTRIBUTES( BOOST_MINSIZE )
    bool set_priority( priority const new_priority ) noexcept
    {
#   ifdef __EMSCRIPTEN__
        if constexpr ( true )
            return ( new_priority == priority::normal );
#   endif
#   ifdef __ANDROID__
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
#   endif
        auto const nice_value( static_cast<int>( new_priority ) );
        bool success( true );
        for ( auto & thread : pool_ )
        {
#   ifdef BOOST_HAS_PTHREADS
#       if defined( __ANDROID__ )
            success &= ( ::setpriority( PRIO_PROCESS, thread.native_handle(), nice_value ) == 0 );
#       else
            std::uint8_t const api_range            ( static_cast<std::int8_t>( priority::idle ) - static_cast<std::int8_t>( priority::time_critical ) );
            auto         const platform_range       ( detail::default_policy_priority_range );
            auto         const uninverted_nice_value( static_cast<std::uint8_t>( - ( nice_value - static_cast<std::int8_t>( priority::idle ) ) ) );
            int          const priority_value       ( detail::default_policy_priority_min + detail::round_divide( uninverted_nice_value * platform_range, api_range ) ); // surely it will be hoisted
#           if defined( __APPLE__ )
            BOOST_ASSERT( !detail::default_policy_priority_unchangable );
            ::sched_param scheduling_parameters;
            int           policy;
            auto const handle( thread.native_handle() );
            BOOST_VERIFY( pthread_getschedparam( handle, &policy, &scheduling_parameters ) == 0 );
            scheduling_parameters.sched_priority = priority_value;
            success &= ( pthread_setschedparam( handle, policy, &scheduling_parameters ) == 0 );
#           else
            success &= !detail::default_policy_priority_unchangable && ( pthread_setschedprio( thread.native_handle(), priority_value ) == 0 );
#           endif
#       endif // __ANDROID__
#   else // thread backend
            /// \note SetThreadPriority() silently falls back to the highest
            /// priority level available to the caller based on its privileges
            /// (instead of failing).
            ///                               (23.06.2017.) (Domagoj Saric)
            BOOST_VERIFY( ::SetThreadPriority( thread.native_handle(), nice_value ) != false );
            success &= true;
#   endif // thread backend
        }

#   if defined( __linux ) // also on Android
        if ( !success )
        {
            success = true;
            spread_the_sweat
            (
                hardware_concurrency_max,
                [ &success, nice_value ]( iterations_t, [[ maybe_unused ]] iterations_t const thread_index ) noexcept
                {
#               if BOOST_SWEATER_USE_CALLER_THREAD
                    /// \note Do not change the caller thread's priority.
                    ///                       (05.05.2017.) (Domagoj Saric)
                    if ( thread_index != hardware_concurrency_max )
#               endif
                    {
                        auto const result( ::setpriority( PRIO_PROCESS, 0, nice_value ) );
                        BOOST_ASSERT( ( result == 0 ) || ( errno == EACCES ) );
                        success &= ( result == 0 );
                    }
                }
            );
        }
#   endif // __linux
        return success;
    }

    BOOST_ATTRIBUTES( BOOST_COLD )
    void bind_worker_to_cpu( hardware_concurrency_t const worker_index, unsigned const cpu_id ) noexcept
    {
        auto & thread{ pool_[ worker_index ] };
#   ifdef _WIN32
        BOOST_VERIFY( ::SetThreadAffinityMask( thread.get_handle(), DWORD_PTR( 1 ) << cpu_id ) != 0 );
#   else // platform
        cpu_set_t cpuset;
        CPU_ZERO( &cpuset );
        CPU_SET( cpu_id, &cpuset );
#       if 0 // Android does not have pthread_setaffinity_np or pthread_attr_setaffinity_np
         // and there seems to be no way of detecting its presence.
        pthread_setaffinity_np( thread.get_id(), sizeof( cpuset ), &cpuset );
#       elif defined( __ANDROID__ ) && ( __ANDROID_API__ >= 21 )
        BOOST_VERIFY( sched_setaffinity( pthread_gettid_np( thread.get_id() ), sizeof( cpuset ), &cpuset ) == 0 );
#       else
        // TODO can be done more efficiently for the BOOST_SWEATER_EXACT_WORKER_SELECTION case
        spread_the_sweat
        (
            number_of_workers(),
            [ &, target_handle = thread.get_id() ]( iterations_t, iterations_t ) noexcept
            {
                if ( pthread_self() == target_handle )
                {
                    BOOST_VERIFY( sched_setaffinity( ::gettid(), sizeof( cpuset ), &cpuset ) == 0 );
                }
            }
        );
#       endif // platform
#   endif // platform
    }

    BOOST_ATTRIBUTES( BOOST_COLD )
    void set_max_allowed_threads( hardware_concurrency_t const max_threads )
    {
        BOOST_ASSERT_MSG( queue_.empty(), "Cannot change parallelism level while items are in queue." );
        stop_and_destroy_pool();
        brexit_.store( false, std::memory_order_relaxed );
        create_pool( max_threads - BOOST_SWEATER_USE_CALLER_THREAD );
    }

private:
    BOOST_ATTRIBUTES( BOOST_COLD )
    void create_pool( hardware_concurrency_t const size )
    {
        BOOST_ASSERT_MSG( size <= sweater::detail::get_hardware_concurrency_max(), "Requested parallelism level not offered in hardware." );
        auto const current_size( pool_.size() );
        BOOST_ASSUME( current_size == 0 );
#   if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        pool_.resize( size );
#   else
        auto p_workers( std::make_unique<worker_thread[]>( size ) );
        pool_ = make_iterator_range_n( p_workers.get(), size );
#   endif // !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY

        for ( hardware_concurrency_t worker_index{ 0 }; worker_index < size; ++worker_index )
            pool_[ worker_index ] = worker_loop( worker_index );
#   if !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        p_workers.release();
#   endif // !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
    }

    BOOST_ATTRIBUTES( BOOST_COLD )
    void stop_and_destroy_pool() noexcept
    {
        {
            std::unique_lock<mutex> lock( mutex_ );
            brexit_.store( true, std::memory_order_relaxed );
            wake_all_workers();
        }
        for ( auto & worker : pool_ )
            worker.join();
#   if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        pool_.clear();
#   else
        delete[] pool_.begin();
        pool_ = {};
#   endif // BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
    }

    BOOST_NOINLINE
    iterations_t // mask for the current iteration count (for branchless setting to zero)
    BOOST_CC_REG enqueue
    (
        work_t                 * __restrict const dispatched_work_parts,
        hardware_concurrency_t              const number_of_dispatched_work_parts,
        barrier                & __restrict       completion_barrier
    ) noexcept
    {
        auto const enqueue_succeeded( queue_.enqueue_bulk( std::make_move_iterator( dispatched_work_parts ), number_of_dispatched_work_parts ) );
        for ( hardware_concurrency_t work_part( 0 ); work_part < number_of_dispatched_work_parts; ++work_part )
            dispatched_work_parts[ work_part ].~work_t();
        if ( BOOST_LIKELY( enqueue_succeeded ) )
        {
            std::unique_lock<mutex> lock( mutex_ );
            if ( BOOST_LIKELY( number_of_dispatched_work_parts == number_of_workers() ) )
            {
                wake_all_workers();
            }
            else
            {
                for ( auto part( 0U ); part < number_of_dispatched_work_parts; ++part )
                {
#               if BOOST_SWEATER_EXACT_WORKER_SELECTION
                    pool_[ part ].event.notify_one();
#               else
                    work_event_.notify_one();
#               endif
                }
            }
            return static_cast<iterations_t>( -1 );
        }
        else
        {
            /// \note If enqueue failed perform everything on the caller's
            /// thread.
            ///                               (21.01.2017.) (Domagoj Saric)
            completion_barrier.mark_as_arrived_to_by_all();
            return static_cast<iterations_t>( 0 );
        }
    }

    template <typename Functor, typename ... Args>
    bool create_fire_and_destroy( Args && ... args ) noexcept
    (
        std::is_nothrow_constructible_v<Functor, Args && ...> &&
        !work_t::requiresAllocation<Functor>
    )
    {
        static_assert( noexcept( std::declval<Functor &>()() ), "Fire and forget work has to be noexcept" );

        bool enqueue_succeeded;
        auto const requiresAllocation( work_t::requiresAllocation<Functor> );
        if constexpr( requiresAllocation )
        {
            struct self_destructed_work
            {
                self_destructed_work( Args && ... args ) : p_functor( new Functor{ std::forward<Args>( args )... } ) {}
                self_destructed_work( self_destructed_work && other ) noexcept : p_functor( other.p_functor ) { other.p_functor = nullptr; BOOST_ASSERT( p_functor ); }
                self_destructed_work( self_destructed_work const & ) = delete;
                void operator()() noexcept( noexcept( std::declval<Functor &>()() ) )
                {
                    BOOST_ASSERT( p_functor );
                    struct destructor
                    {
                        Functor * const p_work;
                        ~destructor() noexcept { delete p_work; }
                    } const eh_safe_destructor{ p_functor };
                    (*p_functor)();
                #ifndef NDEBUG
                    p_functor = nullptr;
                #endif // NDEBUG
                } // void operator()
                Functor * __restrict p_functor = nullptr;
            }; // struct self_destructed_work
            static_assert( std::is_trivially_destructible_v<self_destructed_work> );
            enqueue_succeeded = this->queue_.enqueue( self_destructed_work( std::forward<Args>( args )... ) );
        }
        else
        {
            struct self_destructed_work
            {
                self_destructed_work( Args && ... args ) { new ( storage ) Functor{ std::forward<Args>( args )... }; }
                self_destructed_work( self_destructed_work && other ) noexcept( std::is_nothrow_move_constructible_v<Functor> )
                {
                    auto & source( reinterpret_cast<Functor &>( other.storage ) );
                    new ( storage ) Functor( std::move( source ) );
                    source.~Functor();
                }
                self_destructed_work( self_destructed_work const & ) = delete;
                void operator()() noexcept( noexcept( std::declval<Functor &>()() ) )
                {
                    auto & work( reinterpret_cast<Functor &>( storage ) );
                    struct destructor
                    {
                        Functor & work;
                        ~destructor() noexcept { work.~Functor(); }
                    } eh_safe_destructor{ work };
                    work();
                } // void operator()
                alignas( alignof( Functor ) ) char storage[ sizeof( Functor ) ];
            }; // struct self_destructed_work
            enqueue_succeeded = this->queue_.enqueue( self_destructed_work( std::forward<Args>( args )... ) );
        }
        this->wake_one_worker();
        return BOOST_LIKELY( enqueue_succeeded );
    }

    void wake_one_worker() noexcept
    {
        /// No need for a branch here as the worker thread has to handle
        /// spurious wakeups anyway.
#   if BOOST_SWEATER_EXACT_WORKER_SELECTION
        //...mrmlj...TODO find a idle worker...
        pool_.front().event.notify_one();
#   else
        work_event_.notify_one();
#   endif
    }

    void wake_all_workers() noexcept
    {
        /// No need for a branch here as the worker thread has to handle
        /// spurious wakeups anyway.
#   if BOOST_SWEATER_EXACT_WORKER_SELECTION
        for ( auto & worker : pool_ )
            worker.event.notify_one();
#   else
        work_event_.notify_one();
#   endif
    }

private:
    std::atomic<bool>  brexit_ = ATOMIC_FLAG_INIT;
    mutex              mutex_;
#if BOOST_SWEATER_EXACT_WORKER_SELECTION
    struct alignas( 64 ) worker_thread : thread
    {
        using thread::operator=;
        condition_variable event;
    };
#else
    using worker_thread = thread;
    condition_variable work_event_;
#endif

    /// \todo Further queue refinements.
    /// https://en.wikipedia.org/wiki/Work_stealing
    /// http://www.drdobbs.com/parallel/writing-lock-free-code-a-corrected-queue/210604448
    /// https://github.com/cameron314/readerwriterqueue
    /// http://moodycamel.com/blog/2013/a-fast-lock-free-queue-for-c++
    /// http://stackoverflow.com/questions/1164023/is-there-a-production-ready-lock-free-queue-or-hash-implementation-in-c#14936831
    /// https://github.com/facebook/folly/blob/master/folly/docs/ProducerConsumerQueue.md
    /// https://github.com/facebook/folly/blob/master/folly/MPMCQueue.h
    /// http://landenlabs.com/code/ring/ring.html
    /// https://github.com/Qarterd/Honeycomb/blob/master/src/common/Honey/Thread/Pool.cpp
    ///                                       (12.10.2016.) (Domagoj Saric)
    my_queue queue_;

#if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
    using pool_threads_t = container::static_vector<worker_thread, BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY - BOOST_SWEATER_USE_CALLER_THREAD>;
#else
    using pool_threads_t = iterator_range<worker_thread *>;
#endif
    pool_threads_t pool_;
}; // class shop

#ifdef BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
BOOST_OVERRIDABLE_MEMBER_SYMBOL
std::uint32_t shop::spin_count = 1 * 1000 * 1000;
#endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION

//------------------------------------------------------------------------------
} // namespace generic
//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // generic_hpp
