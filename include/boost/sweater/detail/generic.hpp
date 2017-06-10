////////////////////////////////////////////////////////////////////////////////
///
/// \file generic.hpp
/// -----------------
///
/// (c) Copyright Domagoj Saric 2016 - 2017.
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
#include <boost/sweater/queues/mpmc_moodycamel.hpp>

#include <boost/config_ex.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <boost/functionoid/functionoid.hpp>
#include <boost/range/iterator_range_core.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <future>
#include <iterator>
#ifdef BOOST_MSVC
#include <malloc.h>
#else
#include <alloca.h>
#endif // _MSC_VER
#include <memory>
#include <mutex>
#include <thread>

#ifdef BOOST_HAS_PTHREADS
#include <pthread.h>
#else
#include <windows.h> // for SetThreadPriority
#endif // BOOST_HAS_PTHREADS

#if defined( __linux )
#include <sys/time.h>
#include <sys/resource.h>
#endif // __linux
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------

#ifndef BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
#   define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 0
#endif // BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY

namespace queues { template <typename Work> class mpmc_moodycamel; }

#if defined( __ANDROID__ ) || defined( __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ )
using hardware_concurrency_t = std::uint_fast8_t;
#else
using hardware_concurrency_t = std::uint_fast16_t; // e.g. Intel MIC
#endif

BOOST_OVERRIDABLE_SYMBOL
auto const hardware_concurrency( static_cast<hardware_concurrency_t>( std::thread::hardware_concurrency() ) );

#if defined( __linux ) && !defined( __ANDROID__ ) || defined( __APPLE__ )
namespace detail
{
    BOOST_OVERRIDABLE_SYMBOL auto const default_policy_priority_min        ( ::sched_get_priority_min( SCHED_OTHER ) );
    BOOST_OVERRIDABLE_SYMBOL auto const default_policy_priority_max        ( ::sched_get_priority_max( SCHED_OTHER ) );
    BOOST_OVERRIDABLE_SYMBOL auto const default_policy_priority_range      ( static_cast<std::uint8_t>( default_policy_priority_max - default_policy_priority_min ) );
    BOOST_OVERRIDABLE_SYMBOL auto const default_policy_priority_unchangable( default_policy_priority_range == 0 );

    inline
    std::uint8_t round_divide( std::uint16_t const numerator, std::uint8_t const denominator ) noexcept
    {
        auto const integral_division     ( numerator / denominator );
        auto const atleast_half_remainder( ( numerator % denominator ) >= ( denominator / 2 ) );
        return integral_division + atleast_half_remainder;
    }
} // namespace detail
#endif // __linux && !__ANDROID__ || __APPLE__

class impl
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

        static constexpr std::uint8_t sbo_alignment = 16;

        using empty_handler = functionoid::assert_on_empty;
    }; // struct worker_traits

    using worker_counter = std::atomic<hardware_concurrency_t>;

#ifdef BOOST_HAS_PTHREADS
    //...mrmlj...native threading implementation (avoid the std::bloat)...to be cleaned up and moved to a separate lib...
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
        ~pthread_mutex() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( ::pthread_mutex_destroy( &mutex_ ) == 0 ); }

        pthread_mutex( pthread_mutex && other ) noexcept : mutex_( other.mutex_ ) { other.mutex_ = PTHREAD_MUTEX_INITIALIZER; }
        pthread_mutex( pthread_mutex const & ) = delete;

        void   lock() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( ::pthread_mutex_lock  ( &mutex_ ) == 0 ); }
        void unlock() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( ::pthread_mutex_unlock( &mutex_ ) == 0 ); }

        bool try_lock() noexcept BOOST_NOTHROW_LITE { return ::pthread_mutex_trylock( &mutex_ ) == 0; }

    private: friend class pthread_condition_variable;
        ::pthread_mutex_t mutex_;
    }; // class pthread_mutex

    class pthread_condition_variable
    {
    public:
         pthread_condition_variable() noexcept BOOST_NOTHROW_LITE : cv_( PTHREAD_COND_INITIALIZER ) {}
        ~pthread_condition_variable() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( ::pthread_cond_destroy( &cv_ ) == 0 ); }

        pthread_condition_variable( pthread_condition_variable && other ) noexcept : cv_( other.cv_ ) { other.cv_ = PTHREAD_COND_INITIALIZER; }
        pthread_condition_variable( pthread_condition_variable const & ) = delete;

        void notify_all() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( ::pthread_cond_broadcast( &cv_ ) == 0 ); }
        void notify_one() noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( ::pthread_cond_signal   ( &cv_ ) == 0 ); }
        void wait( std::unique_lock<pthread_mutex> & lock ) noexcept BOOST_NOTHROW_LITE { BOOST_VERIFY( ::pthread_cond_wait( &cv_, &lock.mutex()->mutex_ ) == 0 ); }

    private:
        ::pthread_cond_t cv_;
    }; // class pthread_condition_variable

    using mutex              = pthread_mutex;
    using condition_variable = pthread_condition_variable;
#else
    using mutex              = std::mutex;
    using condition_variable = std::condition_variable;
#endif // BOOST_HAS_PTHREADS

    class batch_semaphore
    {
    public:
        batch_semaphore( hardware_concurrency_t const initial_value ) noexcept : counter_( initial_value ) {}

        BOOST_NOINLINE
        void release() noexcept
        {
            std::unique_lock<mutex> lock( mutex_ );
            if ( counter_.fetch_sub( 1, std::memory_order_relaxed ) == 1 )
                event_.notify_one();
        }

        BOOST_NOINLINE
        void wait() noexcept
        {
#       ifdef BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
            for ( auto try_count( 0U ); try_count < spin_count; ++try_count )
            {
                bool const all_workers_done( counter_.load( std::memory_order_relaxed ) == 0 );
                if ( BOOST_LIKELY( all_workers_done ) )
                {
                    /// \note Lock/wait on the mutex to make sure another thread
                    /// is not right in release() between the counter decrement
                    /// and event_.notify() before we exit this function (after
                    /// which the batch_semaphore destructor is possibly called
                    /// causing the event_.notify() call on the other thread to
                    /// be called on a dead object).
                    ///                       (26.01.2017.) (Domagoj Saric)
                    std::unique_lock<mutex>{ mutex_ };
                    return;
                }
            }
#       endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
            std::unique_lock<mutex> lock{ mutex_ };
            while ( BOOST_UNLIKELY( counter_.load( std::memory_order_relaxed ) != 0 ) )
                event_.wait( lock );
        }

        void reset() noexcept { counter_.store( 0, std::memory_order_release ); }

    private:
        mutex              mutex_  ;
        condition_variable event_  ;
        worker_counter     counter_;
    }; // struct batch_semaphore

    class spread_setup
    {
    public:
        spread_setup( iterations_t const iterations ) noexcept
            :
            iterations_per_worker          ( iterations / impl::number_of_workers() ),
            threads_with_extra_iteration   ( iterations % impl::number_of_workers() - leave_one_for_the_calling_thread() ),
            number_of_dispatched_work_parts( number_of_work_parts( iterations ) - 1 ),
            semaphore                      ( number_of_dispatched_work_parts )
        {
            BOOST_ASSERT( leave_one_for_the_calling_thread() == false || iterations < iterations_t( impl::number_of_workers() ) );
#       if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
            BOOST_ASSUME( number_of_dispatched_work_parts < BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY );
#       endif
        }

        iterations_t           const iterations_per_worker;
        hardware_concurrency_t const threads_with_extra_iteration;
        hardware_concurrency_t const number_of_dispatched_work_parts;
        batch_semaphore              semaphore;

    private:
        // If iterations < workers prefer using the caller thread instead of waking up a worker thread...
        bool leave_one_for_the_calling_thread() const noexcept { return iterations_per_worker == 0; }
        static hardware_concurrency_t number_of_work_parts( iterations_t const iterations ) noexcept
        {
            return static_cast<hardware_concurrency_t>( std::min<iterations_t>( impl::number_of_workers(), iterations ) );
        }
    }; // class spread_setup

    using work_t = functionoid::callable<void(), worker_traits>;

    using my_queue = queues::mpmc_moodycamel<work_t>;

public:
    BOOST_ATTRIBUTES( BOOST_COLD )
    impl()
#if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
    : pool_( BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY - 1 )
#endif
    {
        /// \note Avoid the static-initialization-order-fiasco by not using the
        /// global hardware_concurrency variable (i.e. allow users to safely
        /// create plain global-variable sweat_shop singletons).
        ///                                   (01.05.2017.) (Domagoj Saric)
        auto const local_hardware_concurrency( static_cast<hardware_concurrency_t>( std::thread::hardware_concurrency() ) );
#   if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        BOOST_ASSUME( local_hardware_concurrency <= BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY );
#   else
        auto const number_of_worker_threads( local_hardware_concurrency - 1 );
        auto p_workers( std::make_unique<std::thread[]>( number_of_worker_threads ) );
        pool_ = make_iterator_range_n( p_workers.get(), number_of_worker_threads );
#   endif // !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        for ( auto & worker : pool_ )
        {
            auto const worker_loop
            (
                [this]() noexcept
                {
                    auto token( queue_.consumer_token() );

                    work_t work;

                    for ( ; ; )
                    {
#                   ifdef BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
                        auto const dequeue_cost( 2048 );
                        for ( auto try_count( 0U ); try_count < ( spin_count / dequeue_cost ); ++try_count )
                        {
                            if ( BOOST_LIKELY( queue_.dequeue( work, token ) ) )
                            {
                                work();
                                try_count = 0; // restart the spin-wait
                            }
                        }
#                   endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION

                        bool have_work;
                        {
                            std::unique_lock<mutex> lock( mutex_ );
                            if ( BOOST_UNLIKELY( brexit_.load( std::memory_order_relaxed ) ) )
                                return;
                            /// \note No need for another loop here as a
                            /// spurious-wakeup would be handled by the check in
                            /// the loop above.
                            ///               (08.11.2016.) (Domagoj Saric)
                            have_work = queue_.dequeue( work, token );
                            if ( BOOST_UNLIKELY( !have_work ) )
                                work_event_.wait( lock );
                        }
                        if ( BOOST_LIKELY( have_work ) )
                            work();
                    }
                }
            ); // worker_loop

            worker = std::thread( worker_loop );
        }
#   if !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        p_workers.release();
#   endif // !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
    }

    BOOST_ATTRIBUTES( BOOST_COLD )
    ~impl() noexcept
    {
        {
            std::unique_lock<mutex> lock( mutex_ );
            brexit_.store( true, std::memory_order_relaxed );
            work_event_.notify_all();
        }
        for ( auto & worker : pool_ )
            worker.join();
#   if !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        delete[] pool_.begin();
#   endif // BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
    }

    static hardware_concurrency_t number_of_workers() noexcept
    {
#   if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        BOOST_ASSUME( hardware_concurrency <= BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY );
#   endif
        return hardware_concurrency - unused_cores;
    }

    /// For GCD dispatch_apply/OMP-like parallel loops.
    /// \details Guarantees that <VAR>work</VAR> will not be called more than
    /// <VAR>iterations</VAR> times (even if number_of_workers() > iterations).
    template <typename F>
    bool spread_the_sweat( iterations_t const iterations, F && __restrict work ) noexcept
    {
        static_assert( noexcept( work( iterations, iterations ) ), "F must be noexcept" );

        if ( BOOST_UNLIKELY( iterations == 0 ) )
            return true;

        if ( BOOST_UNLIKELY( iterations == 1 ) )
        {
            work( 0, 1 );
            return true;
        }

        spread_setup setup( iterations );

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
        auto const dispatched_work_parts( static_cast<work_t *>( alloca( setup.number_of_dispatched_work_parts * sizeof( work_t ) ) ) );
#   else
        alignas( work_t ) char dispatched_work_parts_storage[ setup.number_of_dispatched_work_parts * sizeof( work_t ) ];
        auto * const BOOST_MAY_ALIAS dispatched_work_parts( reinterpret_cast<work_t *>( dispatched_work_parts_storage ) );
#   endif // BOOST_MSVC

        iterations_t iteration( 0 );
        for ( hardware_concurrency_t work_part( 0 ); work_part < setup.number_of_dispatched_work_parts; ++work_part )
        {
            auto const start_iteration( iteration );
            auto const extra_iteration( work_part < setup.threads_with_extra_iteration );
            auto const end_iteration  ( static_cast<iterations_t>( start_iteration + setup.iterations_per_worker + extra_iteration ) );
            auto const placeholder( &dispatched_work_parts[ work_part ] );
#       ifdef BOOST_MSVC
            // MSVC14.1 still generates a branch w/o this (GCC issues a warning that it knows that &placeholder cannot be null so this has to be ifdef-guarded).
            BOOST_ASSUME( placeholder );
#       endif // BOOST_MSVC
            auto & semaphore( setup.semaphore );
            new ( placeholder ) work_t
            (
                [&work, start_iteration = iteration, end_iteration, &semaphore]() noexcept
                {
                    work( start_iteration, end_iteration );
                    semaphore.release();
                }
            );
            iteration = end_iteration;
        }

        auto const enqueue_failure_iteration_mask
        (
            enqueue( dispatched_work_parts, setup.number_of_dispatched_work_parts, setup.semaphore )
        );
        iteration &= enqueue_failure_iteration_mask;

        auto const caller_thread_start_iteration( iteration );
        BOOST_ASSERT( caller_thread_start_iteration < iterations );
        work( caller_thread_start_iteration, iterations );

        setup.semaphore.wait();

        return enqueue_failure_iteration_mask != 0;
    }

    template <typename F>
    bool fire_and_forget( F && work ) noexcept( noexcept( std::is_nothrow_constructible<std::remove_reference_t<F>, F &&>::value ) )
    {
        struct self_destructed_work
        {
            self_destructed_work( F       && work ) noexcept( std::is_nothrow_move_constructible<F>::value ) { new ( storage ) F( std::move( work ) ); }
            self_destructed_work( F const &  work ) noexcept( std::is_nothrow_copy_constructible<F>::value ) { new ( storage ) F(            work   ); }
            void operator()() noexcept
            {
                auto & work( reinterpret_cast<F &>( storage ) );
                static_assert( noexcept( work() ), "Work must provide the no-fail exception guarantee" );
                work();
                work.~F();
            }
            alignas( work ) char storage[ sizeof( work ) ];
        };
        auto const enqueue_succeeded( queue_.enqueue( self_destructed_work( std::forward<F>( work ) ) ) );
        /// No need for a branch here as the worker thread has to handle
        /// spurious wakeups anyway.
        std::unique_lock<mutex> lock( mutex_ );
        work_event_.notify_one();
        return BOOST_LIKELY( enqueue_succeeded );
    }

    template <typename F>
    auto dispatch( F && work )
    {
        // http://scottmeyers.blogspot.hr/2013/03/stdfutures-from-stdasync-arent-special.html
        using result_t = typename std::result_of<F()>::type;
        std::promise<result_t> promise;
        std::future<result_t> future( promise.get_future() );
        auto const dispatch_succeeded
        (
            fire_and_forget
            (
                [promise = std::move( promise ), work = std::forward<F>( work )]
                () mutable { promise.set_value( work() ); }
            )
        );
        if ( BOOST_UNLIKELY( !dispatch_succeeded ) )
            future.set_exception( std::make_exception_ptr( std::bad_alloc() ) );
        return future;
    }

#ifdef BOOST_SWEATER_ADJUSTABLE_PARALLELISM
    static void set_number_of_unused_cores( hardware_concurrency_t const number_of_unused_cores ) noexcept
    {
        BOOST_ASSERT_MSG( number_of_unused_cores < hardware_concurrency, "No one left to sweat?" );
        unused_cores = number_of_unused_cores;
    }
#endif // BOOST_SWEATER_ADJUSTABLE_PARALLELISM

#ifdef BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
    static void set_idle_suspend_spin_count( std::uint32_t const new_spin_count ) noexcept
    {
        spin_count = new_spin_count;
    }
#endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION

    BOOST_ATTRIBUTES( BOOST_MINSIZE )
    bool set_priority( priority const new_priority ) noexcept
    {
    #ifdef __ANDROID__
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
    #endif
        auto const nice_value( static_cast<int>( new_priority ) );
        bool success( true );
        for ( auto & thread : pool_ )
        {
        #ifdef BOOST_HAS_PTHREADS
            #if defined( __ANDROID__ )
                success &= ( ::setpriority( PRIO_PROCESS, thread.native_handle(), nice_value ) == 0 );
            #else
                std::uint8_t const api_range            ( static_cast<std::int8_t >( priority::idle ) - static_cast<std::int8_t>( priority::time_critical ) );
                auto         const platform_range       ( detail::default_policy_priority_range );
                auto         const uninverted_nice_value( static_cast<std::uint8_t>( - ( nice_value - static_cast<std::int8_t>( priority::idle ) ) ) );
                int          const priority_value       ( detail::default_policy_priority_min + detail::round_divide( uninverted_nice_value * platform_range, api_range ) ); // surely it will be hoisted
                #if defined( __APPLE__ )
                    BOOST_ASSERT( !detail::default_policy_priority_unchangable );
                    ::sched_param scheduling_parameters;
                    int           policy;
                    auto const handle( thread.native_handle() );
                    BOOST_VERIFY( pthread_getschedparam( handle, &policy, &scheduling_parameters ) == 0 );
                    scheduling_parameters.sched_priority = priority_value;
                    success &= ( pthread_setschedparam( handle, policy, &scheduling_parameters ) == 0 );
                #else
                    success &= !detail::default_policy_priority_unchangable && ( pthread_setschedprio( thread.native_handle(), priority_value ) == 0 );
                #endif
            #endif // __ANDROID__
        #else
            success &= ( ::SetThreadPriority( thread.native_handle(), nice_value ) != false );
        #endif // thread backend
        }

    #if defined( __linux ) && !defined( __ANDROID__ )
        if ( !success )
        {
            success = true;
            spread_the_sweat
            (
                hardware_concurrency,
                [ &success, nice_value ]( iterations_t, iterations_t const thread_index ) noexcept
                {
                    /// \note Do not change the caller thread's priority.
                    ///                       (05.05.2017.) (Domagoj Saric)
                    if ( thread_index != hardware_concurrency )
                        success &= ( ::setpriority( PRIO_PROCESS, 0, nice_value ) == 0 );
                }
            );
        }
    #endif // __linux && !__ANDROID__
        return success;
    }

private:
    BOOST_NOINLINE
    iterations_t // mask for the current iteration count (for branchless setting to zero)
    BOOST_CC_REG enqueue( work_t * __restrict const dispatched_work_parts, hardware_concurrency_t const number_of_dispatched_work_parts, batch_semaphore & __restrict semaphore ) noexcept
    {
        auto const enqueue_succeeded( queue_.enqueue_bulk( std::make_move_iterator( dispatched_work_parts ), number_of_dispatched_work_parts ) );
        for ( hardware_concurrency_t work_part( 0 ); work_part < number_of_dispatched_work_parts; ++work_part )
            dispatched_work_parts[ work_part ].~work_t();
        if ( BOOST_LIKELY( enqueue_succeeded ) )
        {
            std::unique_lock<mutex> lock( mutex_ );
            work_event_.notify_all();
            return static_cast<iterations_t>( -1 );
        }
        else
        {
            /// \note If enqueue failed perform everything on the caller's
            /// thread.
            ///                               (21.01.2017.) (Domagoj Saric)
            semaphore.reset();
            return static_cast<iterations_t>( 0 );
        }
    }

private:
    std::atomic<bool>  brexit_ = ATOMIC_FLAG_INIT;
    mutex              mutex_;
    condition_variable work_event_;

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
    using pool_threads_t = container::static_vector<std::thread, BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY - 1>; // also sweat on the calling thread
#else
    using pool_threads_t = iterator_range<std::thread *>;
#endif
    pool_threads_t pool_;

#ifdef BOOST_SWEATER_ADJUSTABLE_PARALLELISM
    static hardware_concurrency_t unused_cores;
#else
    static hardware_concurrency_t constexpr unused_cores = 0;
#endif // BOOST_SWEATER_ADJUSTIBLE_PARALLELISM
}; // class impl

#ifdef BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
BOOST_OVERRIDABLE_MEMBER_SYMBOL
std::uint32_t impl::spin_count = 1 * 1000 * 1000;
#endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION

#ifdef BOOST_SWEATER_ADJUSTABLE_PARALLELISM
BOOST_OVERRIDABLE_MEMBER_SYMBOL
hardware_concurrency_t impl::unused_cores( 0 );
#endif // BOOST_SWEATER_ADJUSTABLE_PARALLELISM

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // generic_hpp
