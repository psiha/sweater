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
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <boost/functionoid/functionoid.hpp>
#include <boost/range/algorithm/count_if.hpp>
#include <boost/range/iterator_range_core.hpp>

#include <atomic>
#include <cstdint>
#include <future>
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

#ifndef BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
#	define BOOST_SWEATER_MAX_HARDWARE_CONCURENCY 0
#endif // BOOST_SWEATER_MAX_HARDWARE_CONCURENCY

inline auto hardware_concurency() noexcept { return static_cast<std::uint8_t>( std::thread::hardware_concurrency() ); }

struct impl
{
	impl()
#if BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
    : pool_( BOOST_SWEATER_MAX_HARDWARE_CONCURENCY - 1 )
#endif
	{
    #if !BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
        auto const number_of_worker_threads( hardware_concurency() - 1 );
        auto p_workers( std::make_unique<worker[]>( number_of_worker_threads ) );
        pool_ = make_iterator_range_n( p_workers.get(), number_of_worker_threads );
    #endif // !BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
		for ( auto & worker : pool_ )
		{
			auto const worker_loop
			(
				[&worker, this]() noexcept
				{
                    auto & __restrict my_work_available( worker.have_work );
                    auto & __restrict my_work          ( worker.work  );
                    auto & __restrict my_event         ( worker.event );

                    for ( ; ; )
                    {
                        if ( BOOST_UNLIKELY( brexit_.load( std::memory_order_relaxed ) ) )
							return;

                    #ifdef __ANROID__
                        // https://petewarden.com/2015/10/11/one-weird-trick-for-faster-android-multithreading
                        auto constexpr spin_count( 30 * 1000 * 1000 );
                    #else
                        auto constexpr spin_count(                1 );
                    #endif // __ANROID__

                        for ( auto try_count( 0 ); try_count < spin_count; ++try_count )
                        {
                            if ( BOOST_LIKELY( my_work_available.load( std::memory_order_acquire ) ) )
                            {
                                my_work();
                                my_work.clear();
                                /// \note The
                                /// std::condition_variable::notify_one() call
                                /// below should imply a release so we can get
                                /// away with a relaxed store here.
                                ///           (14.10.2016.) (Domagoj Saric)
                                my_work_available.store( false, std::memory_order_relaxed );
                                my_event.notify_one();
                            }
                        }
                        worker.wait( mutex_ );
                    }
				}
			); // worker_loop
            worker.thread = std::thread( worker_loop );
		}
    #if !BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
        p_workers.release();
    #endif // !BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
	}

	~impl() noexcept
	{
		brexit_.store( true, std::memory_order_relaxed );
		for ( auto & worker : pool_ )
        {
            worker.event .notify_one();
			worker.thread.join      ();
        }
    #if !BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
        delete[] pool_.begin();
    #endif // BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
	}

	auto number_of_workers() const { return static_cast<std::uint16_t>( pool_.size() + 1 ); }

    /// For GCD dispatch_apply/OMP-like parallel loops
	template <typename F>
	void spread_the_sweat( std::uint16_t const iterations, F && __restrict work ) noexcept
	{
		static_assert( noexcept( work( iterations, iterations ) ), "F must be noexcept" );

		auto const number_of_workers           ( this->number_of_workers() );
		auto const iterations_per_worker       ( iterations / number_of_workers );
		auto const threads_with_extra_iteration( iterations % number_of_workers );

		std::uint16_t iteration( 0 );
		iteration = spread_iterations
		(
			0, threads_with_extra_iteration,
			iteration, iterations_per_worker + 1,
			work
		);
		iteration = spread_iterations
		(
			threads_with_extra_iteration, pool_.size(),
			iteration, iterations_per_worker,
			work
		);

		auto const caller_thread_start_iteration( iteration );
		BOOST_ASSERT( caller_thread_start_iteration <= iterations );
		work( caller_thread_start_iteration, iterations );

		join();
	}

	template <typename F>
	static void fire_and_forget( F && work )
	{
		std::thread( std::forward<F>( work ) ).detach();
	}

    template <typename F>
    static auto dispatch( F && work )
    {
        // http://scottmeyers.blogspot.hr/2013/03/stdfutures-from-stdasync-arent-special.html
        return std::async( std::launch::async | std::launch::deferred, std::forward<F>( work ) );
    }

private:
	void join() noexcept
	{
        for ( auto & worker : pool_ )
        {
            while ( worker.have_work.load( std::memory_order_acquire ) )
                worker.wait( mutex_ );
        }
	}

    template <typename F>
	std::uint16_t spread_iterations
	(
		std::uint8_t const begin_thread,
		std::uint8_t const end_thread,
		std::uint16_t const begin_iteration,
		std::uint16_t const iterations_per_worker,
		F && work
	) noexcept
	{
		auto iteration( begin_iteration );
		for ( auto thread_index( begin_thread ); thread_index < end_thread; ++thread_index )
		{
			auto & delegate( pool_[ thread_index ].work );
            auto const end_iteration( iteration + iterations_per_worker );
            delegate = [&work, start_iteration = iteration, end_iteration]() noexcept
            {
				work( start_iteration, end_iteration );
            };
            pool_[ thread_index ].event.notify_one();
			iteration = end_iteration;
		}
		return iteration;
	}

private:
    std::atomic<bool> brexit_ = ATOMIC_FLAG_INIT;
	std::mutex        mutex_;

    struct worker_traits : functionoid::std_traits
    {
        static constexpr auto copyable    = functionoid::support_level::na    ;
        static constexpr auto moveable    = functionoid::support_level::nofail;
        static constexpr auto destructor  = functionoid::support_level::nofail;
        static constexpr auto is_noexcept = true;
        static constexpr auto rtti        = false;

        using empty_handler = functionoid::assert_on_empty;
    }; // struct worker_traits

    struct worker
    {
        std::atomic<bool>                            have_work = ATOMIC_FLAG_INIT;
        functionoid::callable<void(), worker_traits> work  ;
        std::condition_variable                      event ;
        std::thread                                  thread;

        BOOST_NOINLINE
        void wait( std::mutex & mutex ) noexcept { event.wait( std::unique_lock<std::mutex>( mutex ) ); }
	};
#if BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
	using pool_threads_t = container::static_vector<worker, BOOST_SWEATER_MAX_HARDWARE_CONCURENCY - 1>; // also sweat on the calling thread
#else
    using pool_threads_t = iterator_range<worker *>;
#endif
	pool_threads_t pool_;

    /// \todo Implement a work queue.
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
}; // struct impl

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // generic_hpp