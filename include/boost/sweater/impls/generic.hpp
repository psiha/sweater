////////////////////////////////////////////////////////////////////////////////
///
/// \file generic.hpp
/// -----------------
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
#ifndef generic_hpp__99FE2034_248F_4C7D_8CD2_EB2BB8247377
#define generic_hpp__99FE2034_248F_4C7D_8CD2_EB2BB8247377
#pragma once
//------------------------------------------------------------------------------
#include "generic_config.hpp"

#include "../queues/mpmc_moodycamel.hpp"
#include "../threading/barrier.hpp"
#include "../threading/hardware_concurrency.hpp"
#include "../threading/cpp/spin_lock.hpp"
#include "../threading/semaphore.hpp"
#include "../threading/thread.hpp"

#include <boost/core/no_exceptions_support.hpp>
#include <boost/config_ex.hpp>
#if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
#include <boost/container/static_vector.hpp>
#endif // BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
#include <boost/functionoid/functionoid.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#if BOOST_SWEATER_EXACT_WORKER_SELECTION
#include <optional>
#endif
#if !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
#include <span>
#endif // !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
#include <type_traits>
#if 0 // sacrifice standard conformance to avoid the overhead of system_error
#include <system_error>
#endif // disabled
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

using hardware_concurrency_t = thrd_lite::hardware_concurrency_t;

class shop
{
public:
    using iterations_t = std::uint32_t;

#if BOOST_SWEATER_HMP
    static bool hmp;

    struct hmp_clusters_info
    {
        static auto constexpr max_clusters{ 3 }; // big - medium - little / turbo - big - little (Android state of affairs)

        hardware_concurrency_t cores[ max_clusters ];
        float                  power[ max_clusters ]; // 'capacity' as in capacity aware scheduling
    }; // struct hmp_clusters_info

private:
    struct hmp_config
    {
        static auto constexpr max_clusters{ hmp_clusters_info::max_clusters };
        static auto constexpr max_power   { 128 };

        hardware_concurrency_t cores[ max_clusters ];
        std::uint8_t           power[ max_clusters ]; // 'capacity' as in capacity aware scheduling

        std::uint8_t number_of_clusters;
    }; // struct hmp_config

    static hmp_config hmp_clusters;

public:
#else
    static bool constexpr hmp{ false };
#endif

#if BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
    static std::uint32_t worker_spin_count;
#if BOOST_SWEATER_USE_CALLER_THREAD
    static std::uint32_t caller_spin_count;
#endif // BOOST_SWEATER_USE_CALLER_THREAD
#endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION

    static std::uint8_t spread_work_stealing_division;

private:
    struct worker_traits : functionoid::default_traits
    {
        static constexpr auto copyable    = functionoid::support_level::na     ;
        static constexpr auto moveable    = functionoid::support_level::nofail ;
        static constexpr auto destructor  = functionoid::support_level::trivial;
        static constexpr auto is_noexcept = true;
        static constexpr auto rtti        = false;
    }; // struct worker_traits

    // TODO
    // http://www.1024cores.net/home/lock-free-algorithms/eventcounts
    // https://github.com/facebook/folly/blob/master/folly/experimental/EventCount.h

    struct spread_work_base
    {
        void const         * p_work              ;
        iterations_t         start_iteration     ;
        iterations_t         end_iteration       ;
        thrd_lite::barrier * p_completion_barrier;
#   if BOOST_SWEATER_EXACT_WORKER_SELECTION && !defined( _WIN32 ) && !defined( NDEBUG ) && 0 //...mrmlj...only for debugging and overflows work_t's SBO storage
        worker_thread      * p_thread;
#   endif // BOOST_SWEATER_EXACT_WORKER_SELECTION
    }; // struct spread_work_base

    using work_t = functionoid::callable<void(), worker_traits>;

    using my_queue = queues::mpmc_moodycamel<work_t>;

    struct spread_worker_template_traits : worker_traits
    {
        static constexpr auto copyable = functionoid::support_level::trivial;
        static constexpr auto moveable = functionoid::support_level::nofail ;
    }; // struct worker_traits

    using spread_work_template_t = functionoid::callable<void(), spread_worker_template_traits>;

    auto number_of_worker_threads() const noexcept;

    auto worker_loop( hardware_concurrency_t worker_index ) noexcept;

public:
    shop()         ;
   ~shop() noexcept;

    thrd_lite::hardware_concurrency_t number_of_workers() const noexcept;

    /// For GCD dispatch_apply/OMP-like parallel loops.
    /// \details Guarantees that <VAR>work</VAR> will not be called more than
    /// <VAR>iterations</VAR> times (even if number_of_workers() > iterations).
    template <typename F>
    bool spread_the_sweat( iterations_t const iterations, F && __restrict work, iterations_t const parallelizable_iterations_count = 1 ) noexcept
    {
        static_assert( noexcept( work( iterations, iterations ) ), "F must be noexcept" );

        struct spread_wrapper : spread_work_base
        {
            void operator()() noexcept
            {
#           if BOOST_SWEATER_EXACT_WORKER_SELECTION && !defined( _WIN32 ) && !defined( NDEBUG ) && 0 //...mrmlj...todo
                auto const tid{ ::gettid() };
                BOOST_ASSERT( tid == p_thread->thread_id );
#           endif // BOOST_SWEATER_EXACT_WORKER_SELECTION
                BOOST_ASSUME( start_iteration < end_iteration );
                auto & __restrict work{ *static_cast<std::decay_t<F> *>( const_cast< void * >( p_work ) ) };
                work( start_iteration, end_iteration );
                p_completion_barrier->arrive();
#           ifndef NDEBUG
                p_work               = nullptr;
                p_completion_barrier = nullptr;
                start_iteration = end_iteration = static_cast<iterations_t>( -1 );
#           endif // !NDEBUG
            }
        }; // struct spread_wrapper
        static_assert( std::is_standard_layout_v<spread_wrapper> ); // required for correctness of work_t::target_as() usage
#   ifdef BOOST_GCC
#       pragma GCC diagnostic push
#       pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#   endif // GCC
#   ifndef BOOST_MSVC // 16.7 ICE
        BOOST_ASSERT( spread_work_template_t{ spread_wrapper{{ .p_work = &work }} }.target_as<spread_work_base>().p_work == &work );
#   endif // BOOST_MSVC
        return spread_work( spread_wrapper{{ .p_work = &work }}, iterations, parallelizable_iterations_count );
#   ifdef BOOST_GCC
#       pragma GCC diagnostic pop
#   endif // GCC
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
            // Note: Clang v8 and v9 think that result_t is unused
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

    using cpu_affinity_mask = thrd_lite::thread::affinity_mask;

    bool set_priority( thrd_lite::priority new_priority ) noexcept;

    bool bind_worker       ( hardware_concurrency_t worker_index, cpu_affinity_mask ) noexcept;
    bool bind_worker_to_cpu( hardware_concurrency_t worker_index, unsigned cpu_id   ) noexcept;

    void set_max_allowed_threads( hardware_concurrency_t max_threads );

    hardware_concurrency_t number_of_items() const noexcept;

#if BOOST_SWEATER_HMP
    void configure_hmp( hmp_clusters_info config, std::uint8_t number_of_clusters );
#endif // BOOST_SWEATER_HMP

private:
    void create_pool( hardware_concurrency_t size );

    void stop_and_destroy_pool() noexcept;

    void perform_caller_work
    (
        iterations_t                   iterations,
        spread_work_template_t const & work_part_template,
        thrd_lite::barrier           & completion_barrier
    ) noexcept;

#if BOOST_SWEATER_EXACT_WORKER_SELECTION
    auto dispatch_workers
    (
        hardware_concurrency_t         worker_index,
        iterations_t                   iteration,
        hardware_concurrency_t         max_parts,
        iterations_t                   per_part_iterations,
        iterations_t                   parts_with_extra_iteration,
        iterations_t                   iterations, // total/max for the whole spread (not necessary all for this call)
        thrd_lite::barrier           & completion_barrier,
        spread_work_template_t const & work_part_template
    ) noexcept;
#endif // BOOST_SWEATER_EXACT_WORKER_SELECTION

    bool BOOST_CC_REG spread_work
    (
        spread_work_template_t work_part_template,
        iterations_t           iterations,
        iterations_t           parallelizable_iterations_count
    ) noexcept;

    template <typename Functor, typename ... Args>
    bool create_fire_and_destroy( Args && ... args ) noexcept
    (
        std::is_nothrow_constructible_v<Functor, Args && ...> &&
        !work_t::requires_allocation<Functor>
    )
    {
        static_assert( noexcept( std::declval<Functor &>()() ), "Fire and forget work has to be noexcept" );

        bool enqueue_succeeded;
        if constexpr( work_t::requires_allocation<Functor> )
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

#       if BOOST_SWEATER_EXACT_WORKER_SELECTION
            if ( !thrd_lite::slow_thread_signals )
            {
                enqueue_succeeded = this->pool_.front().enqueue( self_destructed_work{ std::forward<Args>( args )... }, this->queue_ );
            }
            else
#       endif
            {
#       if !BOOST_SWEATER_EXACT_WORKER_SELECTION || defined( __ANDROID__ )
                enqueue_succeeded = this->queue_.enqueue( self_destructed_work{ std::forward<Args>( args )... } );
                this->work_semaphore_.signal( 1 );
#       endif
            }
        }
        else
        {
            struct self_destructed_work
            {
                self_destructed_work( Args && ... args ) { new ( storage ) Functor{ std::forward<Args>( args )... }; }
                self_destructed_work( self_destructed_work && other ) noexcept
                (
#               if BOOST_WORKAROUND( BOOST_MSVC, BOOST_TESTED_AT( 1928 ) )
                    true
#               else
                    std::is_nothrow_move_constructible_v<Functor>
#               endif // VS 16.8 workarounds
                )
                {
                    auto & source( reinterpret_cast<Functor &>( other.storage ) );
                    new ( storage ) Functor( std::move( source ) );
                    source.~Functor();
                }
                self_destructed_work( self_destructed_work const & ) = delete;
                void operator()()
#               if !BOOST_WORKAROUND( BOOST_MSVC, BOOST_TESTED_AT( 1928 ) )
                    noexcept( noexcept( std::declval<Functor &>()() ) )
#               endif // VS 16.8 workarounds
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
#       if BOOST_SWEATER_EXACT_WORKER_SELECTION
            if ( !thrd_lite::slow_thread_signals )
            {
                enqueue_succeeded = this->pool_.front().enqueue( self_destructed_work{ std::forward<Args>( args )... }, this->queue_ );
            }
            else
#       endif
            {
#       if !BOOST_SWEATER_EXACT_WORKER_SELECTION || defined( __ANDROID__ )
                enqueue_succeeded = this->queue_.enqueue( self_destructed_work{ std::forward<Args>( args )... } );
                this->work_semaphore_.signal( 1 );
#       endif
            }
        }

        this->work_added( enqueue_succeeded );
        return BOOST_LIKELY( enqueue_succeeded );
    }

    void wake_all_workers() noexcept;

    void work_added    ( hardware_concurrency_t items = 1 ) noexcept;
    void work_completed(                                  ) noexcept;

private:
#if BOOST_SWEATER_EXACT_WORKER_SELECTION
    struct alignas( 64 ) worker_thread : thrd_lite::thread
    {
        using thrd_lite::thread::operator=;

        void notify() noexcept;

        bool enqueue(                     work_t &&                                         , my_queue & ) noexcept;
        bool enqueue( std::move_iterator< work_t * >, hardware_concurrency_t number_of_items, my_queue & ) noexcept;

        thrd_lite::semaphore                      event_;
        thrd_lite::spin_lock                      token_mutex_; // producer tokens are not thread safe (support concurrent spread_the_sweat calls)
        std::optional<my_queue::producer_token_t> token_;
#   ifdef __linux__
        pid_t thread_id_ = 0;
#   endif // Linux
    }; // struct worker_thread

#if defined( __ANDROID__ )
    // Partial fix attempt for slow thread synchronization on older Android
    // versions (it seems to be related to the OS version rather than the
    // kernel version).
    thrd_lite::semaphore work_semaphore_;
#endif // Android
#else // BOOST_SWEATER_EXACT_WORKER_SELECTION
    using worker_thread = thrd_lite::thread;

    thrd_lite::semaphore work_semaphore_;
#endif // BOOST_SWEATER_EXACT_WORKER_SELECTION

    std::atomic<hardware_concurrency_t> work_items_ = 0;
    std::atomic<bool                  > brexit_     = false;

    /// \todo Further queue refinements.
    /// http://ithare.com/implementing-queues-for-event-driven-programs
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

    // Caller work-stealing 'explicit' token (still a question whether worth it).
    thrd_lite::spin_lock       consumer_token_mutex_;
    my_queue::consumer_token_t consumer_token_;

#if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
#   ifdef __ANDROID__
#       define NUM_THREAD_CORRECTIONS 0
#   else
#       define NUM_THREAD_CORRECTIONS BOOST_SWEATER_USE_CALLER_THREAD
#   endif
    using pool_threads_t = container::static_vector<worker_thread, BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY - NUM_THREAD_CORRECTIONS>;
#   undef NUM_THREAD_CORRECTIONS
#else
    using pool_threads_t = std::span<worker_thread>;
#endif
    pool_threads_t pool_;
}; // class shop

//------------------------------------------------------------------------------
} // namespace generic
//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // generic_hpp
