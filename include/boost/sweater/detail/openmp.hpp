////////////////////////////////////////////////////////////////////////////////
///
/// \file openmp.hpp
/// ----------------
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
#ifndef openmp_hpp__A03505C4_4323_437C_A38E_BF26BBCBD143
#define openmp_hpp__A03505C4_4323_437C_A38E_BF26BBCBD143
#pragma once
//------------------------------------------------------------------------------
#include <omp.h>

#include <cstdint>
#include <future>
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

inline auto hardware_concurency() noexcept { return static_cast<std::uint16_t>( omp_get_num_procs() ); }

struct impl
{
	static auto number_of_workers() noexcept { return static_cast<std::uint16_t>( omp_get_max_threads() ); }

	template <typename F>
	static void spread_the_sweat( std::uint16_t const iterations, F & work ) noexcept
	{
		static_assert( noexcept( noexcept( work( iterations, iterations ) ) ), "F must be noexcept" );
		#pragma omp parallel
		{
			auto const number_of_workers( static_cast<std::uint8_t>( omp_get_num_threads() ) );
			auto const iterations_per_worker( iterations / number_of_workers + 1 );

			#pragma omp for
			for ( std::int16_t iteration( 0 ); iteration < iterations; iteration += iterations_per_worker )
			{
				work( iteration, std::min<std::uint16_t>( iterations, iteration + iterations_per_worker ) );
			}
		}
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

}; // struct impl

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
#endif // openmp_hpp