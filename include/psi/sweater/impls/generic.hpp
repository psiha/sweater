////////////////////////////////////////////////////////////////////////////////
///
/// \file generic.hpp
/// -----------------
///
/// (c) Copyright Domagoj Saric 2016 - 2025.
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

#if PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
// Fixed-capacity pool for embedded/mobile targets with a bounded core count.
// Use std::inplace_vector (C++26 / P0843) when available; otherwise the
// caller must supply their own fixed-size container via a PSI_SWEATER_POOL_T
// specialisation.  Desktop targets use MAX == 0 → std::span (heap-allocated).
#  if defined(__cpp_lib_inplace_vector)
#    include <inplace_vector>
#  else
#    include <psi/vm/containers/fc_vector.hpp>
#  endif
#endif // PSI_SWEATER_MAX_HARDWARE_CONCURRENCY

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>   // std::move_only_function
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#if PSI_SWEATER_EXACT_WORKER_SELECTION
#include <optional>
#endif
#if !PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
#include <span>
#endif // !PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
#include <type_traits>
#if 0 // sacrifice standard conformance to avoid the overhead of system_error
#include <system_error>
#endif // disabled
//------------------------------------------------------------------------------
namespace psi::sweater::queues
{ template <typename Work> class mpmc_moodycamel; }
//------------------------------------------------------------------------------
namespace psi::sweater::generic
{
//------------------------------------------------------------------------------

using hardware_concurrency_t = thrd_lite::hardware_concurrency_t;

class shop
{
public:
    using iterations_t = std::uint32_t;

#if PSI_SWEATER_HMP
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

#if PSI_SWEATER_SPIN_BEFORE_SUSPENSION
    static std::uint32_t worker_spin_count;
#if PSI_SWEATER_USE_CALLER_THREAD
    static std::uint32_t caller_spin_count;
#endif // PSI_SWEATER_USE_CALLER_THREAD
#endif // PSI_SWEATER_SPIN_BEFORE_SUSPENSION

    static std::uint8_t spread_work_stealing_division;

private:
    // Work items are noexcept move-only callables (no copy needed in the fast path).
    using work_t = std::move_only_function<void() noexcept>;

    using my_queue = queues::mpmc_moodycamel<work_t>;

    // ── Spread-work support ──────────────────────────────────────────────────
    // A spread_work_entry is a trivially-copyable plain-data struct so that
    // spread_work() can stamp iteration ranges into copies without wrapping
    // in a non-copyable move_only_function first.
    struct spread_work_entry
    {
        void const         * p_work              ;
        iterations_t         start_iteration     ;
        iterations_t         end_iteration       ;
        thrd_lite::barrier * p_completion_barrier;
        void (*invoke)( spread_work_entry const & ) noexcept;

        void operator()() noexcept { invoke( *this ); }
    }; // struct spread_work_entry

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

        auto invoke_fn = []( spread_work_entry const & e ) noexcept
        {
            BOOST_ASSUME( e.start_iteration < e.end_iteration );
            auto & __restrict typed_work{ *static_cast<std::decay_t<F> *>( const_cast<void *>( e.p_work ) ) };
            typed_work( e.start_iteration, e.end_iteration );
            e.p_completion_barrier->arrive();
        };

        spread_work_entry tmpl{
            .p_work               = &work,
            .start_iteration      = 0,
            .end_iteration        = 0,
            .p_completion_barrier = nullptr,
            .invoke               = invoke_fn
        };

        static_assert( std::is_standard_layout_v<spread_work_entry> );
        return spread_work( tmpl, iterations, parallelizable_iterations_count );
    }

    template <typename F>
    bool fire_and_forget( F && work ) noexcept( noexcept( std::is_nothrow_constructible_v<std::remove_reference_t<F>, F &&> ) )
    {
        static_assert( noexcept( std::declval<std::remove_reference_t<F> &>()() ), "Fire and forget work has to be noexcept" );
        return enqueue_work( work_t{ std::forward<F>( work ) } );
    }

    template <typename F>
    auto dispatch( F && work )
    {
        // http://scottmeyers.blogspot.hr/2013/03/stdfutures-from-stdasync-arent-special.html
        using Functor = std::remove_reference_t<F>;
        struct future_wrapper
        {
            using result_t  = decltype( std::declval<Functor &>()() );
            using promise_t = std::promise<result_t>;
            using future_t  = std::future <result_t>;

            future_wrapper( F && work_source, future_t & future )
                : work( std::forward<F>( work_source ) )
            {
                future = promise.get_future();
            }

            void operator()() noexcept
            {
                try
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
                catch( ... )
                {
                    promise.set_exception( std::current_exception() );
                }
            }

            Functor   work   ;
            promise_t promise;
        }; // struct future_wrapper

        typename future_wrapper::future_t future;
        auto const dispatch_succeeded( enqueue_work( work_t{ future_wrapper{ std::forward<F>( work ), future } } ) );
        if ( PSI_UNLIKELY( !dispatch_succeeded ) )
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

#if PSI_SWEATER_HMP
    void configure_hmp( hmp_clusters_info config, std::uint8_t number_of_clusters );
#endif // PSI_SWEATER_HMP

private:
    bool enqueue_work( work_t && work ) noexcept;

    void create_pool( hardware_concurrency_t size );

    void stop_and_destroy_pool() noexcept;

    void perform_caller_work
    (
        iterations_t                    iterations,
        spread_work_entry const       & work_template,
        thrd_lite::barrier            & completion_barrier
    ) noexcept;

#if PSI_SWEATER_EXACT_WORKER_SELECTION
    auto dispatch_workers
    (
        hardware_concurrency_t         worker_index,
        iterations_t                   iteration,
        hardware_concurrency_t         max_parts,
        iterations_t                   per_part_iterations,
        iterations_t                   parts_with_extra_iteration,
        iterations_t                   iterations,
        thrd_lite::barrier           & completion_barrier,
        spread_work_entry const      & work_template
    ) noexcept;
#endif // PSI_SWEATER_EXACT_WORKER_SELECTION

    bool spread_work
    (
        spread_work_entry work_template,
        iterations_t      iterations,
        iterations_t      parallelizable_iterations_count
    ) noexcept;

private:
#if PSI_SWEATER_EXACT_WORKER_SELECTION
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
    thrd_lite::semaphore work_semaphore_;
#endif // Android
#else // PSI_SWEATER_EXACT_WORKER_SELECTION
    using worker_thread = thrd_lite::thread;

    thrd_lite::semaphore work_semaphore_;
#endif // PSI_SWEATER_EXACT_WORKER_SELECTION

    std::atomic<hardware_concurrency_t> work_items_ = 0;
    std::atomic<bool                  > brexit_     = false;

    my_queue queue_;

    // Caller work-stealing 'explicit' token (still a question whether worth it).
    thrd_lite::spin_lock       consumer_token_mutex_;
    my_queue::consumer_token_t consumer_token_;

#if PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
#   ifdef __ANDROID__
#       define NUM_THREAD_CORRECTIONS 0
#   else
#       define NUM_THREAD_CORRECTIONS PSI_SWEATER_USE_CALLER_THREAD
#   endif
    static constexpr auto pool_capacity = PSI_SWEATER_MAX_HARDWARE_CONCURRENCY - NUM_THREAD_CORRECTIONS;
#   undef NUM_THREAD_CORRECTIONS
#   if defined(__cpp_lib_inplace_vector)
    using pool_threads_t = std::inplace_vector<worker_thread, pool_capacity>;
#   else
    using pool_threads_t = psi::vm::fc_vector<worker_thread, pool_capacity>;
#   endif
#else
    using pool_threads_t = std::span<worker_thread>;
#endif
    pool_threads_t pool_;
}; // class shop

//------------------------------------------------------------------------------
} // namespace psi::sweater::generic
//------------------------------------------------------------------------------
#endif // generic_hpp
