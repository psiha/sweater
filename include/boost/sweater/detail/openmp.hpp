////////////////////////////////////////////////////////////////////////////////
///
/// \file openmp.hpp
/// ----------------
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
#ifndef openmp_hpp__A03505C4_4323_437C_A38E_BF26BBCBD143
#define openmp_hpp__A03505C4_4323_437C_A38E_BF26BBCBD143
#pragma once
//------------------------------------------------------------------------------
#include <boost/config_ex.hpp>

#include <omp.h>

#include <cstdint>
#include <future>
#include <thread>
#include <type_traits>
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

#if defined( __ANDROID__ ) || defined( __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ )
using hardware_concurrency_t = std::uint_fast8_t;
#else
using hardware_concurrency_t = std::uint_fast16_t; // e.g. Intel MIC
#endif

BOOST_OVERRIDABLE_SYMBOL
auto const hardware_concurrency( static_cast<hardware_concurrency_t>( std::thread::hardware_concurrency() ) );

struct impl
{
    using iterations_t = std::uint32_t;

    // http://openmp.org/mp-documents/OpenMP_Examples_4.0.1.pdf

	static auto number_of_workers() noexcept { return static_cast<hardware_concurrency_t>( omp_get_max_threads() ); }

	template <typename F>
	static void spread_the_sweat( iterations_t const iterations, F & work ) noexcept
	{
		static_assert( noexcept( work( iterations, iterations ) ), "F must be noexcept" );
		#pragma omp parallel
		{
			auto const number_of_workers( static_cast<hardware_concurrency_t>( omp_get_num_threads() ) );
			auto const iterations_per_worker( iterations / number_of_workers + 1 );

			#pragma omp for
			for ( std::make_signed_t<iterations_t> iteration( 0 ); iteration < iterations; iteration += iterations_per_worker )
			{
				work( iteration, std::min<iterations_t>( iterations, iteration + iterations_per_worker ) );
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