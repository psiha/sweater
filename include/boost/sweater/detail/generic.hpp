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
#include <boost/range/algorithm/count_if.hpp>

#include <cstdint>
#include <future>
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
#	define BOOST_SWEATER_MAX_HARDWARE_CONCURENCY 0;
#endif // BOOST_SWEATER_MAX_HARDWARE_CONCURENCY

inline auto hardware_concurency() noexcept { return static_cast<std::uint8_t>( std::thread::hardware_concurrency() ); }

struct impl
{
	impl()
	{
		auto const actual_threads( hardware_concurency() );
		std::unique_lock<std::mutex> const my_lock( mutex_ );
		for ( std::uint8_t t( 0U ); t < actual_threads - 1; ++t )
		{
			auto const worker_loop
			(
				[t, this]() noexcept
				{
					auto & __restrict the_mutex( mutex_ );
					the_mutex.lock();
					auto & __restrict my_work  ( pool_[ t ].first );
					the_mutex.unlock();

					for ( ; ; )
					{
						{
							std::unique_lock<std::mutex> my_lock( the_mutex );
							while ( !my_work && BOOST_LIKELY( !brexit_ ) )
								event_.wait( my_lock );
						}
						if ( BOOST_UNLIKELY( brexit_ ) )
							return;
						my_work.function( my_work.start_iteration, my_work.end_iteration, my_work.object );
                        std::unique_lock<std::mutex> my_lock( mutex_ );
						my_work.clear();
						event_.notify_all();
					}
				}
			);
			pool_.emplace_back( std::piecewise_construct, std::make_tuple(), std::make_tuple( worker_loop ) );
		}
	}

	~impl() noexcept
	{
		brexit_ = true;
		event_.notify_all();
		for ( auto & worker : pool_ )
			worker.second.join();
	}

	auto number_of_workers() const { return static_cast<std::uint16_t>( pool_.size() + 1 ); }

    /// For GCD dispatch_apply/OMP-like parallel loops
	template <typename F>
	void spread_the_sweat( std::uint16_t const iterations, F & __restrict work ) noexcept
	{
		static_assert( noexcept( noexcept( work( iterations, iterations ) ) ), "F must be noexcept" );
		auto const invoker
		(
			[]( std::uint16_t const start_iteration, std::uint16_t const end_iteration, void * const p_functor ) noexcept
			{
				auto & __restrict f( *static_cast<F *>( p_functor ) );
				f( start_iteration, end_iteration );
			}
		);

		auto const number_of_workers           ( this->number_of_workers() );
		auto const iterations_per_worker       ( iterations / number_of_workers );
		auto const threads_with_extra_iteration( iterations % number_of_workers );

		std::uint16_t iteration( 0 );
		iteration = spread_iterations
		(
			0, threads_with_extra_iteration,
			iteration, iterations_per_worker + 1,
			&work, invoker
		);
		iteration = spread_iterations
		(
			threads_with_extra_iteration, pool_.size(),
			iteration, iterations_per_worker,
			&work, invoker
		);
		event_.notify_all();

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
        return std::async( std::launch::async | std::launch::deferred, std::forward<F>( work ) );
    }

private:
	void join() noexcept
	{
		std::unique_lock<std::mutex> my_lock( mutex_ );
		while ( boost::count_if( pool_, []( auto const & worker ) { return worker.first; } ) )
			event_.wait( my_lock );
	}

	BOOST_NOINLINE
	std::uint16_t spread_iterations
	(
		std::uint8_t const begin_thread,
		std::uint8_t const end_thread,
		std::uint16_t const begin_iteration,
		std::uint16_t const iterations_per_worker,
		void * const p_worker,
		void (*p_function)( std::uint16_t, std::uint16_t, void * )
	) noexcept
	{
		auto iteration( begin_iteration );
		for ( auto thread_index( begin_thread ); thread_index < end_thread; ++thread_index )
		{
			auto & delegate( pool_[ thread_index ].first );
			delegate.start_iteration = iteration;
			delegate.end_iteration   = iteration + iterations_per_worker;
			delegate.object          = p_worker;
			delegate.function        = p_function;
			iteration = delegate.end_iteration;
		}
		return iteration;
	}

private:
	bool volatile           brexit_ = false;
	std::mutex              mutex_;
	std::condition_variable event_;
	// std::function<> breaks down when volatile qualified, and since we don't need its trans-fatty acids:
	struct LightWeightDelegate
	{
		void (* function)( std::uint16_t start_iteration, std::uint16_t end_iteration, void * pFunctor )
		#ifndef _MSC_VER // srsly
            noexcept
        #endif // _MSC_VER
		= nullptr;
		void * volatile object        = nullptr;
		std::uint16_t start_iteration = 0;
		std::uint16_t end_iteration   = 0;

		void clear() { object = nullptr; }
		explicit operator bool() const { return object; }
	}; // struct LightWeightDelegate

#if BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
	using pool_threads_t = container::static_vector
	<
		std::pair
		<
			LightWeightDelegate,
			std::thread
		>,
		BOOST_SWEATER_MAX_HARDWARE_CONCURENCY - 1 // also sweat on the calling thread
	>;
#else
	using pool_threads_t = container::small_vector
	<
		std::pair
		<
			LightWeightDelegate,
			std::thread
		>,
		4 - 1 // also sweat on the calling thread
	>;
#endif
	pool_threads_t pool_;
};

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // generic_hpp