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
#include <cstdint>
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

inline auto hardware_concurency() noexcept { return std::thread::hardware_concurrency(); }

struct impl
{
	impl()
	{
		auto const actual_threads( std::min( std::uint8_t(max_threads), static_cast<std::uint8_t>( std::thread::hardware_concurrency() ) ) );
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

	template <typename F>
	void spread_the_sweat( std::uint16_t const iterations, F & __restrict work ) noexcept
	{
		static_assert( noexcept( noexcept( work( iterations, iterations ) ) ), "F must be noexcept" );
		auto const iterations_per_worker( iterations / number_of_workers() + 1 );
		auto iteration( 0U );
		for ( auto & worker : pool_ )
		{
			auto & delegate( worker.first );
			delegate.start_iteration = iteration;
			delegate.end_iteration   = iteration + iterations_per_worker;
			delegate.object          = &work;
			delegate.function        = []( std::uint16_t const start_iteration, std::uint16_t const end_iteration, void * const pFunctor ) noexcept { auto & __restrict f( *static_cast<F *>( pFunctor ) ); f( start_iteration, end_iteration ); };
			iteration = delegate.end_iteration;
		}
		BOOST_ASSERT( pool_.back().first.end_iteration < iterations );
		event_.notify_all();

		work( pool_.back().first.end_iteration, iterations );

		std::unique_lock<std::mutex> my_lock( mutex_ );
		while ( boost::count_if( pool_, []( auto const & worker ) { return worker.first; } ) )
			event_.wait( my_lock );
	}

	template <typename F>
	void fire_and_forget( F && work ) noexcept
	{
		std::thread( std::forward<F>( work ) ).detach();
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

	ShortVector
	<
		std::pair
		<
			LightWeightDelegate,
			std::thread
		>,
		max_threads - 1 // also sweat on the calling thread
	> pool_;
};

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // generic_hpp