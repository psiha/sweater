////////////////////////////////////////////////////////////////////////////////
///
/// \file apple.hpp
/// ---------------
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
#pragma once
//------------------------------------------------------------------------------
#include <cstdint>
#include <thread>

#include <dispatch/dispatch.h>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------

#ifndef BOOST_SWEATER_MAX_HARDWARE_CONCURENCY
#if defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
#	define BOOST_SWEATER_MAX_HARDWARE_CONCURENCY 3; // iPad 2 Air
#else
#	define BOOST_SWEATER_MAX_HARDWARE_CONCURENCY 0;
#endif
#endif // BOOST_SWEATER_MAX_HARDWARE_CONCURENCY

struct impl
{
	// http://www.idryman.org/blog/2012/08/05/grand-central-dispatch-vs-openmp
	static auto number_of_workers() { return static_cast<std::uint8_t>( std::thread::hardware_concurrency() ); }

	template <typename F>
	void spread_the_sweat( std::uint16_t const iterations, F & work ) noexcept
	{
		static_assert( noexcept( noexcept( work( iterations, iterations ) ) ), "F must be noexcept" );
		auto const number_of_workers( this->number_of_workers() );
		auto const iterations_per_worker( iterations / number_of_workers );
		auto const leftover_iterations( iterations - iterations_per_worker * number_of_workers );

		dispatch_apply
		(
			number_of_workers,
            dispatch_get_global_queue( QOS_CLASS_DEFAULT, 0 ),
			^( std::size_t const worker_index )
			{
				auto       start_iteration( static_cast<std::uint16_t>( worker_index * iterations_per_worker ) );
				auto const stop_iteration ( start_iteration + iterations_per_worker + leftover_iterations );
				if ( worker_index > 0 )
					start_iteration += leftover_iterations;

				work( start_iteration, stop_iteration );
			}
		);
	}

	template <typename F>
	void fire_and_forget( F & work ) noexcept
	{

	}
}; // struct impl

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------