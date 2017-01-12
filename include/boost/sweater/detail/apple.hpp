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
#include <algorithm>
#include <cstdint>
#include <future>
#include <thread>
#include <type_traits>

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
#	define BOOST_SWEATER_MAX_HARDWARE_CONCURENCY 3 // iPad 2 Air
#else
#	define BOOST_SWEATER_MAX_HARDWARE_CONCURENCY 0
#endif
#endif // BOOST_SWEATER_MAX_HARDWARE_CONCURENCY

struct impl
{
	// http://www.idryman.org/blog/2012/08/05/grand-central-dispatch-vs-openmp
	static auto number_of_workers() noexcept { return static_cast<std::uint8_t>( std::thread::hardware_concurrency() ); }

	template <typename F>
	static void spread_the_sweat( std::uint16_t const iterations, F && work ) noexcept
	{
		static_assert( noexcept( work( iterations, iterations ) ), "F must be noexcept" );

        /// \note Stride the iteration count based on the number of workers
        /// (otherwise dispatch_apply will make an indirect function call for
        /// each iteration).
        /// The iterations / number_of_workers is an integer division an can
        /// thus be 'lossy'. We workaround this by adding one in case this
        /// happens and then limiting the end iteration in the worker lambda
        /// (with the effect that the last 'started'/woken thread does the least
        /// work).
        ///                                   (04.10.2016.) (Domagoj Saric)
        auto const number_of_workers( impl::number_of_workers() );
        auto const do_extra_iteration   ( ( iterations % number_of_workers ) != 0 );
        auto const iterations_per_worker(   iterations / number_of_workers + do_extra_iteration );
        auto /*const*/ worker
        (
            [&work, iterations_per_worker, iterations]
            ( std::uint16_t const worker_index ) noexcept
            {
                auto const start_iteration( worker_index * iterations_per_worker );
                auto const stop_iteration ( std::min<std::uint16_t>( start_iteration + iterations_per_worker, iterations ) );

                work( start_iteration, stop_iteration );
            }
        );

        /// \note dispatch_apply delegates to dispatch_apply_f so we avoid the
        /// small overhead of an extra jmp and block construction (as opposed to
        /// just a trivial lambda construction).
        ///                                   (04.10.2016.) (Domagoj Saric)
        /// \note The iteration_per_worker logic above does not fully cover the
        /// cases where the number of iterations is less than the number of
        /// workers (resulting in work being called with start_iteration >
        /// stop_iteration) so we have to additionally clamp the iterations
        /// parameter passed to dispatch_apply).
        ///                                   (12.01.2017.) (Domagoj Saric)
		dispatch_apply_f
		(
			std::min<std::uint16_t>( number_of_workers, iterations ),
            dispatch_get_global_queue( QOS_CLASS_DEFAULT, 0 ),
            &worker,
            []( void * const p_context, std::size_t const worker_index ) noexcept
            {
                auto & __restrict the_worker( *static_cast<decltype( worker ) const *>( p_context ) );
                the_worker( static_cast<std::uint16_t>( worker_index ) );
            }
		);
	}

	template <typename F>
	static void fire_and_forget( F && work ) noexcept
	{
        /// \note "ObjC++ attempts to copy lambdas, preventing capture of
        /// move-only types". https://llvm.org/bugs/show_bug.cgi?id=20534
        ///                                   (14.01.2016.) (Domagoj Saric)
        __block auto moveable_work( std::forward<F>( work ) );
        dispatch_async
		(
            dispatch_get_global_queue( QOS_CLASS_DEFAULT, 0 ),
			^() { moveable_work(); }
		);
	}

    template <typename F>
    static auto dispatch( F && work )
    {
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
}; // struct impl

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------