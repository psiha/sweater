////////////////////////////////////////////////////////////////////////////////
///
/// \file generic.hpp
/// -----------------
///
/// (c) Copyright Domagoj Saric 2016.
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
#include <boost/range/algorithm/count_if.hpp>
#include <boost/range/iterator_range_core.hpp>

#include <atomic>
#include <cstdint>
#include <future>
#include <iterator>
#ifdef _MSC_VER
#include <malloc.h>
#else
#include <alloca.h>
#endif // _MSC_VER
#include <memory>
#include <mutex>
#include <thread>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------

#ifndef BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
#	define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 0
#endif // BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY

namespace queues { template <typename Work> class mpmc_moodycamel; }

BOOST_OVERRIDABLE_SYMBOL
auto const hardware_concurrency( static_cast<std::uint8_t>( std::thread::hardware_concurrency() ) );

class impl
{
private:
#ifdef __ANROID__
    // https://petewarden.com/2015/10/11/one-weird-trick-for-faster-android-multithreading
    static auto constexpr spin_count = 30 * 1000 * 1000;
#else
    static auto constexpr spin_count =                1;
#endif // __ANROID__

    struct worker_traits : functionoid::std_traits
    {
        static constexpr auto copyable    = functionoid::support_level::na     ;
        static constexpr auto moveable    = functionoid::support_level::nofail ;
        static constexpr auto destructor  = functionoid::support_level::trivial;
        static constexpr auto is_noexcept = true;
        static constexpr auto rtti        = false;

        using empty_handler = functionoid::assert_on_empty;
    }; // struct worker_traits

    using worker_counter = std::atomic<std::uint16_t>;

    class batch_semaphore
    {
    public:
        batch_semaphore( std::uint16_t const initial_value ) : counter( initial_value ) {}

        void release() noexcept
        {
            std::unique_lock<std::mutex> lock( mutex );
            if ( counter.fetch_sub( 1, std::memory_order_acquire ) == 1 )
                event.notify_one();
        }

        BOOST_NOINLINE
        void wait() noexcept
        {
            for ( auto try_count( 0 ); try_count < spin_count; ++try_count )
            {
                bool const all_workers_done( counter.load( std::memory_order_relaxed ) == 0 );
                if ( BOOST_LIKELY( all_workers_done ) )
                    return;
            }
            std::unique_lock<std::mutex> lock( mutex );
            while ( BOOST_UNLIKELY( counter.load( std::memory_order_relaxed ) != 0 ) )
                event.wait( lock );
        }

    private:
                worker_counter          counter;
        mutable std::condition_variable event;
                std::mutex              mutex;
    }; // struct batch_semaphore

    struct work_data
    {
        work_data            (                    ) = default;
        work_data            ( work_data const &  ) = default;
        work_data            ( work_data       && ) = default;
        work_data & operator=( work_data const &  ) = default;
        work_data & operator=( work_data       && ) = default;
        functionoid::callable<void(), worker_traits> callback;
        batch_semaphore * __restrict                 p_semaphore;

        void do_work()
        {
            callback();
            if ( p_semaphore )
                p_semaphore->release();
        }
    }; // struct work_data

    using my_queue = queues::mpmc_moodycamel<work_data>;

public:
	impl()
#if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
    : pool_( BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY - 1 )
#endif
	{
    #if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        BOOST_ASSUME( hardware_concurrency <= BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY );
    #else
        auto const number_of_worker_threads( hardware_concurrency - 1 );
        auto p_workers( std::make_unique<std::thread[]>( number_of_worker_threads ) );
        pool_ = make_iterator_range_n( p_workers.get(), number_of_worker_threads );
    #endif // !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
		for ( auto & worker : pool_ )
		{
			auto const worker_loop
			(
				[this]() noexcept
				{
                    auto token( queue_.consumer_token() );

                    work_data work;

                    for ( ; ; )
                    {
                        for ( auto try_count( 0 ); try_count < spin_count; ++try_count )
                        {
                            if ( BOOST_LIKELY( queue_.dequeue( work, token ) ) )
                            {
                                work.do_work();
                                if ( spin_count > 1 ) // restart the spin-wait
                                    try_count = 0;
                            }
                        }

                        if ( BOOST_UNLIKELY( brexit_.load( std::memory_order_relaxed ) ) )
							return;

                        {
                            std::unique_lock<std::mutex> lock( mutex_ );
                            /// \note No need for a another loop here as a
                            /// spurious-wakeup would be handled by the check in the
                            /// loop above.
                            ///                   (08.11.2016.) (Domagoj Saric)
                            if ( !BOOST_LIKELY( queue_.dequeue( work, token ) ) )
                            {
                                work.callback.clear();
                                work_event_.wait( lock );
                            }
                        }
                        if ( BOOST_LIKELY( static_cast<bool>( work.callback ) ) )
                            work.do_work();
                    }
				}
			); // worker_loop
            worker = std::thread( worker_loop );
		}
    #if !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        p_workers.release();
    #endif // !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
	}

	~impl() noexcept
	{
		brexit_.store( true, std::memory_order_relaxed );
        work_event_.notify_all();
		for ( auto & worker : pool_ )
			worker.join();
    #if !BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        delete[] pool_.begin();
    #endif // BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
	}

	auto number_of_workers() const
    {
        auto const result( static_cast<std::uint16_t>( pool_.size() + 1 ) );
    #if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        BOOST_ASSUME( result <= BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY );
    #endif
        return result;
    }

    /// For GCD dispatch_apply/OMP-like parallel loops.
    /// \details Guarantees that <VAR>work</VAR> will not be called more than
    /// <VAR>iterations</VAR> times (even if number_of_workers() > iterations).
	template <typename F>
	bool spread_the_sweat( std::uint16_t const iterations, F && __restrict work ) noexcept
	{
		static_assert( noexcept( work( iterations, iterations ) ), "F must be noexcept" );

        if ( BOOST_UNLIKELY( iterations == 0 ) )
            return true;

        auto const number_of_workers               ( this->number_of_workers()      );
		auto const iterations_per_worker           ( iterations / number_of_workers );
        auto const leave_one_for_the_calling_thread( iterations_per_worker == 0     ); // If iterations < workers prefer using the caller thread instead of waking up a worker thread...
		auto const threads_with_extra_iteration    ( iterations % number_of_workers - leave_one_for_the_calling_thread );
        BOOST_ASSERT( !leave_one_for_the_calling_thread || iterations < number_of_workers );

        auto const number_of_work_parts( std::min<std::uint16_t>( number_of_workers, iterations ) );
        std::uint16_t const number_of_dispatched_work_parts( number_of_work_parts - 1 );

        batch_semaphore semaphore( number_of_dispatched_work_parts );

#   if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        BOOST_ASSUME( number_of_dispatched_work_parts < BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY );
#   else
        BOOST_ASSUME( number_of_dispatched_work_parts < 512 );
#   endif
        auto const dispatched_work_parts( static_cast<work_data *>( alloca( ( number_of_dispatched_work_parts ) * sizeof( work_data ) ) ) );

        std::uint16_t iteration( 0 );
        if ( BOOST_LIKELY( iterations > 1 ) )
        {
            for ( std::uint8_t work_part( 0 ); work_part < number_of_dispatched_work_parts; ++work_part )
            {
                auto const start_iteration( iteration );
                auto const extra_iteration( work_part < threads_with_extra_iteration );
                auto const end_iteration  ( start_iteration + iterations_per_worker + extra_iteration );
                new ( &dispatched_work_parts[ work_part ] ) work_data
                {
                    [&work, start_iteration = iteration, end_iteration]() noexcept
                    { work( start_iteration, end_iteration ); },
                    &semaphore
                };
                iteration = end_iteration;
            }

            auto const enqueue_succeeded( queue_.enqueue_bulk( std::make_move_iterator( dispatched_work_parts ), number_of_dispatched_work_parts ) );
            for ( std::uint8_t work_part( 0 ); work_part < number_of_dispatched_work_parts; ++work_part )
                dispatched_work_parts[ work_part ].~work_data();
            if ( !BOOST_LIKELY( enqueue_succeeded ) )
                return false;
            std::unique_lock<std::mutex> lock( mutex_ );
            work_event_.notify_all();
        }

		auto const caller_thread_start_iteration( iteration );
		BOOST_ASSERT( caller_thread_start_iteration < iterations );
		work( caller_thread_start_iteration, iterations );

        if ( BOOST_LIKELY( iterations > 1 ) )
        {
            semaphore.wait();
        }

        return true;
	}

	template <typename F>
	void fire_and_forget( F && work )
	{
        struct self_destructed_work
        {
            self_destructed_work( F       && work ) noexcept( std::is_nothrow_move_constructible<F>::value ) { new ( storage ) F( std::move( work ) ); }
            self_destructed_work( F const &  work ) noexcept( std::is_nothrow_copy_constructible<F>::value ) { new ( storage ) F(            work   ); }
            void operator()() noexcept
            {
                auto & work( reinterpret_cast<F &>( storage ) );
                work();
                work.~F();
            }
            alignas( work ) char storage[ sizeof( work ) ];
        };
        queue_.enqueue( work_data{ self_destructed_work( std::forward<F>( work ) ), nullptr } );
	}

    template <typename F>
    auto dispatch( F && work )
    {
        // http://scottmeyers.blogspot.hr/2013/03/stdfutures-from-stdasync-arent-special.html
        using result_t = typename std::result_of<F()>::type;
        std::promise<result_t> promise;
        std::future<result_t> future( promise.get_future() );
        fire_and_forget
        (
            [promise = std::move( promise ), work = std::forward<F>( work )]
            () mutable { promise.set_value( work() ); }
        );
        return future;
    }

private:
            std::atomic<bool>       brexit_ = ATOMIC_FLAG_INIT;
	        std::mutex              mutex_;
    mutable std::condition_variable work_event_;

#if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
	using pool_threads_t = container::static_vector<std::thread, BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY - 1>; // also sweat on the calling thread
#else
    using pool_threads_t = iterator_range<std::thread *>;
#endif
	pool_threads_t pool_;

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
}; // class impl

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // generic_hpp