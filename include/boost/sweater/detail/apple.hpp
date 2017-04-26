////////////////////////////////////////////////////////////////////////////////
///
/// \file apple.hpp
/// ---------------
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
#pragma once
//------------------------------------------------------------------------------
#include <boost/assert.hpp>
#include <boost/config_ex.hpp>

#include <algorithm>
#include <cstdint>
#include <future>
#include <thread>
#include <type_traits>

#include <dispatch/dispatch.h>
#include <TargetConditionals.h>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------

#ifndef BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
#if TARGET_OS_IOS
#   ifdef __aarch64__
#	    define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 3 // iPad 2 Air
#   else
#       define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 2
#   endif
#else
#	define BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY 0
#endif
#endif // BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY

BOOST_OVERRIDABLE_SYMBOL
auto const hardware_concurrency( static_cast<std::uint8_t>( std::thread::hardware_concurrency() ) );

class impl
{
public:
    using iterations_t = std::uint32_t;

	// http://www.idryman.org/blog/2012/08/05/grand-central-dispatch-vs-openmp
	static auto number_of_workers() noexcept
    {
        BOOST_ASSERT_MSG( hardware_concurrency == std::thread::hardware_concurrency(), "Hardware concurrency changed at runtime!?" );
    #if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        BOOST_ASSUME( hardware_concurrency <= BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY );
    #endif
        return hardware_concurrency;
    }

	template <typename F>
	static void spread_the_sweat( iterations_t const iterations, F && work ) noexcept
	{
		static_assert( noexcept( work( iterations, iterations ) ), "F must be noexcept" );

        /// \note Stride the iteration count based on the number of workers
        /// (otherwise dispatch_apply will make an indirect function call for
        /// each iteration).
        /// The iterations / number_of_workers is an integer division and can
        /// thus be 'lossy' so extra steps need to be taken to account for this.
        ///                                   (04.10.2016.) (Domagoj Saric)
        auto         const number_of_workers    ( impl::number_of_workers() );
        iterations_t const iterations_per_worker( iterations / number_of_workers );
        std::uint8_t const extra_iterations     ( iterations % number_of_workers );
        auto /*const*/ worker
        (
            [
                &work, iterations_per_worker, extra_iterations
            #ifndef NDEBUG
                , iterations
            #endif // !NDEBUG
            ]
            ( std::uint8_t const worker_index ) noexcept
            {
                auto const extra_iters        ( std::min( worker_index, extra_iterations ) );
                auto const plain_iters        ( worker_index - extra_iters                 );
                auto const this_has_extra_iter( worker_index < extra_iterations            );
                auto const start_iteration
                (
                    extra_iters * ( iterations_per_worker + 1 )
                        +
                    plain_iters *   iterations_per_worker
                );
                auto const stop_iteration( start_iteration + iterations_per_worker + this_has_extra_iter );
                BOOST_ASSERT( stop_iteration <= iterations );
                BOOST_ASSERT_MSG( start_iteration < stop_iteration, "Sweater internal inconsistency: worker called with no work to do." );
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
			std::min<iterations_t>( number_of_workers, iterations ),
            high_priority_queue,
            &worker,
            []( void * const p_context, std::size_t const worker_index ) noexcept
            {
                auto & __restrict the_worker( *static_cast<decltype( worker ) const *>( p_context ) );
                the_worker( static_cast<std::uint8_t>( worker_index ) );
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
        dispatch_async( high_priority_queue, ^(){ moveable_work(); } );
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

private:
    static dispatch_queue_t const default_queue      ;
    static dispatch_queue_t const high_priority_queue;
}; // struct impl

__attribute__(( weak )) dispatch_queue_t const impl::default_queue      ( dispatch_get_global_queue( QOS_CLASS_DEFAULT       , 0 ) );
__attribute__(( weak )) dispatch_queue_t const impl::high_priority_queue( dispatch_get_global_queue( QOS_CLASS_USER_INITIATED, 0 ) );

//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
