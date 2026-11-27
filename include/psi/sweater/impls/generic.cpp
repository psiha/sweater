////////////////////////////////////////////////////////////////////////////////
///
/// \file generic.cpp
/// -----------------
///
/// (c) Copyright Domagoj Saric 2016 - 2023.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "generic.hpp"
#include <psi/build/attributes.hpp>
#include "../dispatch_tracking.hpp"

#include <algorithm>
#include <bit>
#include <new>
#include <thread>
//------------------------------------------------------------------------------
#if PSI_SWEATER_EXACT_WORKER_SELECTION && defined( _WIN32 ) && !defined( _WIN64 )
struct _IMAGE_DOS_HEADER;
extern "C" _IMAGE_DOS_HEADER __ImageBase;
#endif
//------------------------------------------------------------------------------
namespace psi::sweater::generic
{
//------------------------------------------------------------------------------

#ifdef __clang__
// Clang does not support [[ (un)likely ]] cpp attributes
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
#endif // clang

namespace events
{
#if defined( __GNUC__ )
#   define WEAK __attribute__(( weak ))
#else
#   define WEAK
#endif

    WEAK void caller_join_begin        ( bool /*spinning*/                                                                                 ) noexcept {}
    WEAK void caller_join_end          (                                                                                                   ) noexcept {}
    WEAK void caller_work_begin        (                                          std::uint32_t /*iterations*/                             ) noexcept {}
    WEAK void caller_work_end          (                                                                                                   ) noexcept {}
    WEAK void worker_work_begin        ( hardware_concurrency_t /*worker_index*/                                                           ) noexcept {}
    WEAK void worker_work_end          ( hardware_concurrency_t /*worker_index*/                                                           ) noexcept {}
    WEAK void worker_sleep_begin       ( hardware_concurrency_t /*worker_index*/                                                           ) noexcept {}
    WEAK void worker_sleep_end         ( hardware_concurrency_t /*worker_index*/                                                           ) noexcept {}
    WEAK void worker_bulk_enqueue_begin( hardware_concurrency_t /*number_of_workers*/                                                      ) noexcept {}

    WEAK void spread_begin             ( std::uint32_t /*iterations*/                                                                      ) noexcept {}
    WEAK void spread_preexisting_work  (                                          hardware_concurrency_t /*items_in_shop*/                 ) noexcept {}
    WEAK void spread_recursive_call    ( hardware_concurrency_t /*worker_index*/, hardware_concurrency_t /*items_in_shop*/                 ) noexcept {}
    WEAK void spread_end               ( hardware_concurrency_t /*dispatched_parts*/, bool /*caller_used*/                                 ) noexcept {}

    WEAK void worker_thread_init       ( hardware_concurrency_t /*worker_index*/                                                           ) noexcept {}

#undef WEAK
} // namespace events

namespace
{
    // The pool the current thread is a worker of, and which of its workers it
    // is. Asking the shop's item counter instead whether a spread is a
    // recursive one does not work: an item is counted only after it was
    // queued, so it can already be running (and spreading) while the shop
    // still looks empty.
    constinit thread_local shop const *           this_threads_shop  { nullptr };
    constinit thread_local hardware_concurrency_t this_threads_worker{ 0       };

    // Out of line: called once per worker thread, and kept out of the code
    // generated for the worker loop.
    BOOST_NOINLINE
    void this_thread_is_worker( shop const & parent, hardware_concurrency_t const worker_index ) noexcept
    {
        this_threads_shop   = &parent;
        this_threads_worker = worker_index;
    }
} // anonymous namespace

#if PSI_SWEATER_HMP
bool shop::hmp = true;
shop::hmp_config shop::hmp_clusters;
#endif

#if PSI_SWEATER_SPIN_BEFORE_SUSPENSION
#ifdef __ANDROID__
// The historical Android value, tuned against that platform's slow thread
// signals -- deliberately untouched by the general-default change below
// (no Android hardware in the bench fleet to re-tune it on).
std::uint32_t shop::worker_spin_count{ 100 * 1000 };
#else
// Small on purpose -- see generic_config.hpp's spin-before-suspension note:
// ~1k nops captures the catch-the-next-dispatch win (fire flat-to-better,
// back-to-back small-spread join ~30-40% faster on Apple Silicon) while
// large counts regress both by oversubscribing the cores after a join.
std::uint32_t shop::worker_spin_count{ 1000 };
#endif // Android
#if PSI_SWEATER_USE_CALLER_THREAD
std::uint32_t shop::caller_spin_count{ 100 * 1000 };
#endif // PSI_SWEATER_USE_CALLER_THREAD
#endif // PSI_SWEATER_SPIN_BEFORE_SUSPENSION

// A seq_cst read-modify-write is a full barrier on x86; elsewhere it takes a
// fence to order the load that follows it.
static void fence_after_rmw() noexcept
{
#if defined( __x86_64__ ) || defined( __i386__ ) || defined( _M_X64 ) || defined( _M_IX86 )
    std::atomic_signal_fence( std::memory_order_seq_cst );
#else
    std::atomic_thread_fence( std::memory_order_seq_cst );
#endif
}

auto shop::worker_loop( [[ maybe_unused ]] hardware_concurrency_t const worker_index ) noexcept
{
    /// \note PSI_SWEATER_EXACT_WORKER_SELECTION requires the worker
    /// index to be captured by the worker lambda - this makes it larger
    /// than a void pointer and thus requries the 'synchronized_invocation'
    /// path in the create thread call. This in turn causes deadlocks when
    /// shops are created as global variables under
    /// - Windows in DLLs (as the DLL entry procedure then blocks when it
    ///   must not do so). As a workaround the worker index and the shop
    ///   instance pointer are packed together (expecting to never be
    ///   further apart than 2GB, avoiding the 64bit address canonical
    ///   form pointer tagging complexities)
    /// - Emscripten because threads are not created until control is
    ///   yielded back to JS code (and sweat_shop constructor dead locks
    ///   waiting for the workers to start - as control is never returned
    ///   JS).
    /// https://lwn.net/Articles/718888
    /// https://source.android.com/devices/tech/debug/tagged-pointers
    ///                                   (13.08.2020.) (Domagoj Saric)
#if PSI_SWEATER_EXACT_WORKER_SELECTION && ( defined( _WIN64 ) || defined( __LP64__ ) )
    static_assert( sizeof( void * ) == 8 );
    static std::byte /*const - MSVC '& on constant'!?*/ dummy_reference_object{};
    auto const shop_offset{ reinterpret_cast<std::byte const *>( this ) - &dummy_reference_object };
    struct shop_and_worker_t
    {
        std:: int64_t shop_offset  : 48;
        std::uint64_t worker_index : 16;
    } const shop_and_worker{ .shop_offset = shop_offset, .worker_index = worker_index };
    static_assert( sizeof( void * ) == sizeof( shop_and_worker ) );
    BOOST_ASSERT( shop_and_worker.shop_offset  == shop_offset  );
    BOOST_ASSERT( shop_and_worker.worker_index == worker_index );
#elif PSI_SWEATER_EXACT_WORKER_SELECTION
    static_assert( sizeof( void * ) == 4 );
#   ifdef _WIN32
    using global_offset_t = std::uint32_t;
    static std::byte const & dummy_reference_object{ reinterpret_cast<std::byte const &>( __ImageBase ) };
#   else
    using global_offset_t = std::int32_t;
    static std::byte const dummy_reference_object{};
#   endif
    auto const shop_offset{ static_cast<global_offset_t>( reinterpret_cast<std::byte const *>( this ) - &dummy_reference_object ) };
    struct shop_and_worker_t
    {
        global_offset_t shop_offset  : 27;
        std::uint32_t   worker_index :  5;
    } const shop_and_worker{ .shop_offset = shop_offset, .worker_index = worker_index };
    static_assert( sizeof( void * ) == sizeof( shop_and_worker ) );
    BOOST_ASSERT( shop_and_worker.shop_offset  == shop_offset  );
    BOOST_ASSERT( shop_and_worker.worker_index == worker_index );
#else
    auto const p_shop{ this };
#endif // EWS
    auto worker_loop_impl
    {
        [=]() noexcept
        {
#       if PSI_SWEATER_EXACT_WORKER_SELECTION
            auto & parent{ const_cast< shop & >( *reinterpret_cast<shop const *>( &dummy_reference_object + shop_and_worker.shop_offset ) ) };
            auto const worker_index{ static_cast<hardware_concurrency_t>( shop_and_worker.worker_index ) };
#       else
            auto & parent{ *p_shop };
            auto const worker_index{ static_cast<hardware_concurrency_t>( -1 ) };
#       endif // PSI_SWEATER_EXACT_WORKER_SELECTION

#       if PSI_SWEATER_EXACT_WORKER_SELECTION
#           ifdef __linux__
            parent.pool_[ worker_index ].thread_id_ = ::gettid();
#           endif // linux
            auto       & __restrict producer_token{                                   parent.pool_[ worker_index ].token_                          };
#           ifdef __ANDROID__
            auto       & __restrict work_event    { !thrd_lite::slow_thread_signals ? parent.pool_[ worker_index ].event_ : parent.work_semaphore_ };
#           else
            auto       & __restrict work_event    {                                   parent.pool_[ worker_index ].event_                          };
#           endif // Android
#       else // PSI_SWEATER_EXACT_WORKER_SELECTION
            auto       & __restrict work_event    { parent.work_semaphore_ };
#       endif // PSI_SWEATER_EXACT_WORKER_SELECTION
            auto       & __restrict queue         { parent.queue_  };
            auto const & __restrict exit          { parent.brexit_ };

            auto consumer_token{ queue.consumer_token() };

            work_t work;

            auto const run
            {
                [ & ]() noexcept
                {
#               if PSI_SWEATER_EXACT_WORKER_SELECTION
                    // Work queued on this worker since it last looked now
                    // waits behind the item about to run - possibly for long
                    // - so an idle worker is woken to steal it. The
                    // store -> fence -> token check half of the pairing with
                    // enqueue_on()'s signal -> fence -> executing_ load:
                    // either the enqueuer sees this worker busy or this sees
                    // its token (a token for work that was stolen meanwhile
                    // just wakes a worker that finds nothing).
                    auto & self{ parent.pool_[ worker_index ] };
                    if ( !thrd_lite::slow_thread_signals )
                    {
                        self.executing_.exchange( true, std::memory_order_seq_cst );
                        fence_after_rmw();
                        if ( work_event.try_wait() ) [[ unlikely ]]
                            parent.wake_idle( 1, worker_index );
                    }
#               endif // EWS
                    events::worker_work_begin( worker_index );
                    work();
                    parent.work_completed();
                    events::worker_work_end  ( worker_index );
#               if PSI_SWEATER_EXACT_WORKER_SELECTION
                    // No fence needed: a worker is not asleep or about to be
                    // until it announces it (see the idle set), and the
                    // announcement is what pairs with the producers.
                    self.executing_.store( false, std::memory_order_relaxed );
#               endif // EWS
                }
            };

            // One-shot per-worker init hook (weak no-op unless a consumer
            // overrides it — e.g. rama installs mimalloc's mi_thread_init).
            events::worker_thread_init( worker_index );

            this_thread_is_worker( parent, worker_index );

            for ( ; ; )
            {
#           if PSI_SWEATER_EXACT_WORKER_SELECTION
                while
                (
                    !thrd_lite::slow_thread_signals &&
                    PSI_LIKELY( queue.dequeue_from_producer( work, *producer_token ) )
                ) [[ likely ]]
                {
                    run();
                }
#           endif // EWS
                // Work stealing for EWS
                while ( queue.dequeue( work, consumer_token ) ) [[ likely ]]
                {
                    run();
                }

#           if PSI_SWEATER_EXACT_WORKER_SELECTION
                // A signal that is already waiting is a look at the queue that
                // somebody asked for: take it and look, without anything
                // else (the common case of a stream of items, each of which
                // signals).
                if ( !thrd_lite::slow_thread_signals && work_event.try_wait() )
                    continue;
#           endif // EWS
                if ( PSI_UNLIKELY( exit.load( std::memory_order_relaxed ) ) )
                    return;
                // Neither sleep past an item that is still queued: the
                // dequeues above can miss one that concurrent dequeues are in
                // the middle of taking (they only look empty), and every
                // producer has signalled someone else. Announce that this
                // worker waits (see the idle set) before looking once more:
                // either an item that is queued meanwhile is seen here, or
                // its producer sees the announcement and signals this worker.
#           if PSI_SWEATER_EXACT_WORKER_SELECTION
                if ( !thrd_lite::slow_thread_signals )
                {
                    parent.set_idle( worker_index );
                    if ( !queue.empty() ) [[ unlikely ]]
                    {
                        parent.withdraw_idle( worker_index );
                        std::this_thread::yield();
                        continue;
                    }
                }
                else
#           endif // EWS
                if ( !queue.empty() ) [[ unlikely ]]
                {
                    std::this_thread::yield();
                    continue;
                }
                events::worker_sleep_begin( worker_index );
#           if PSI_SWEATER_SPIN_BEFORE_SUSPENSION
                work_event.wait( worker_spin_count );
#           else
                work_event.wait();
#           endif // PSI_SWEATER_SPIN_BEFORE_SUSPENSION
                events::worker_sleep_end  ( worker_index );
#           if PSI_SWEATER_EXACT_WORKER_SELECTION
                // Woken (by a claim, which cleared the bit, or by a signal sent
                // to this worker in particular, which did not).
                if ( !thrd_lite::slow_thread_signals )
                {
                    parent.withdraw_idle( worker_index );
                    // Freshly woken with work still in flight: continue the
                    // spread wake tree (see propagate_spread_wake) before
                    // starting to consume. Keyed off the shop-wide item
                    // counter, which can be ahead of what the queue shows
                    // (moodycamel consumers can observe a spuriously empty
                    // queue while items exist). A false positive (items that
                    // are merely executing) costs a look at two idle bits.
                    if ( parent.number_of_items() != 0 )
                        parent.propagate_spread_wake( worker_index );
                }
#           endif // EWS
            }
        }
    }; // worker_loop_impl
#if defined( _WIN32 ) || defined( __EMSCRIPTEN__ )
    static_assert
    (
        thrd_lite::detail::fits_into_a_pointer< decltype( worker_loop_impl ) >,
        "This worker_loop_impl will cause a dead lock in the constructor of global shops inside DLLs under Windows or under Emscripten"
    );
#endif // Windows || Emscripten
    return worker_loop_impl;
}

auto shop::number_of_worker_threads() const noexcept
{
    auto const worker_threads{ static_cast<hardware_concurrency_t>( pool_.size() ) };
#if PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
    BOOST_ASSUME( worker_threads <= PSI_SWEATER_MAX_HARDWARE_CONCURRENCY );
#endif
    return worker_threads;
}

shop::shop()
    :
    consumer_token_{ queue_.consumer_token() }
{
#ifdef __GNUC__ // compilers with init_priority attribute (see hardware_concurency.hpp)
    hardware_concurrency_t local_hardware_concurrency( thrd_lite::hardware_concurrency_max );
#else
    /// \note Avoid the static-initialization-order-fiasco (for compilers
    /// not supporting the init_priority attribute) by not using the
    /// global hardware_concurrency_max variable (i.e. allow users to
    /// safely create plain global-variable sweat_shop singletons).
    ///                                   (01.05.2017.) (Domagoj Saric)
    hardware_concurrency_t local_hardware_concurrency( thrd_lite::get_hardware_concurrency_max() );
#endif // __GNUC__
#if PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
    /// \note Fail-safe for possible future devices that may overflow
    /// PSI_SWEATER_MAX_HARDWARE_CONCURRENCY or for cases like running an
    /// ARMv7 slice on modern ARMv8 hardware.
    ///                                   (14.12.2021.) (Domagoj Saric)
    local_hardware_concurrency = std::min<hardware_concurrency_t>( local_hardware_concurrency, PSI_SWEATER_MAX_HARDWARE_CONCURRENCY );
#endif
    BOOST_ASSUME( local_hardware_concurrency > 0 );
    create_pool( local_hardware_concurrency - PSI_SWEATER_USE_CALLER_THREAD );
}

shop::~shop() noexcept { stop_and_destroy_pool(); }

hardware_concurrency_t shop::number_of_workers() const noexcept
{
    auto const actual_number_of_workers{ number_of_worker_threads() + PSI_SWEATER_USE_CALLER_THREAD };
#if PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
    BOOST_ASSUME( actual_number_of_workers <= PSI_SWEATER_MAX_HARDWARE_CONCURRENCY );
#endif
    return static_cast<hardware_concurrency_t>( actual_number_of_workers );
}

// https://web.archive.org/web/20161026022836/http://www.route32.net/2016/09/ticks-scheduling-and-confighz-overview.html
// https://unix.stackexchange.com/questions/466722/how-to-change-the-length-of-time-slices-used-by-the-linux-cpu-scheduler

[[gnu::optimize("Os")]]
bool shop::set_priority( thrd_lite::priority const new_priority ) noexcept
{
#ifdef __EMSCRIPTEN__
    return new_priority == thrd_lite::priority::normal;
#else // !Emscripten
    [[ maybe_unused ]] auto const nice_value( static_cast<int>( new_priority ) );
    bool success( true );
    for ( auto & thread : pool_ )
    {
        success &= thread.set_priority( new_priority );
    }

#if defined( __linux ) // also on Android
    if ( !success )
    {
#   if PSI_SWEATER_HMP // Disable HMP to get all workers queried
        auto hmp_setting{ hmp };
        hmp = false;
#   endif // PSI_SWEATER_HMP
#   if PSI_SWEATER_USE_CALLER_THREAD
        auto const caller_id{ ::gettid() };
#   endif // PSI_SWEATER_USE_CALLER_THREAD
        success = true;
        spread_the_sweat
        (
            thrd_lite::hardware_concurrency_max,
            [ =, &success ]( iterations_t, iterations_t /*worker index?*/ ) noexcept
            {
#           if PSI_SWEATER_USE_CALLER_THREAD
                /// \note Do not change the caller thread's priority.
                ///                       (05.05.2017.) (Domagoj Saric)
                if ( ::gettid() != caller_id )
#           endif // PSI_SWEATER_USE_CALLER_THREAD
                {
                    auto const result( ::setpriority( PRIO_PROCESS, 0, nice_value ) );
                    BOOST_ASSERT( ( result == 0 ) || ( errno == EACCES ) );
                    success &= ( result == 0 );
                }
            }
        );
#   if PSI_SWEATER_HMP
        hmp = hmp_setting;
#   endif // PSI_SWEATER_HMP
    }
#endif // __linux
    return success;
#endif // !Emscripten
}

PSI_COLD
bool shop::bind_worker
(
    hardware_concurrency_t const worker_index,
    cpu_affinity_mask      const mask
) noexcept
{
    if ( worker_index >= number_of_worker_threads() ) // e.g. an empty pool (a parallelism of one)
        return false;
    auto & thread{ pool_[ worker_index ] };
    auto succeeded{ thread.bind_to_cpu( mask ) };
    if ( !succeeded )
    {
#   if defined( __linux__ )
#       if PSI_SWEATER_EXACT_WORKER_SELECTION
        succeeded |= thrd_lite::thread::bind_to_cpu( thread.thread_id_, mask );
#       else
#       if PSI_SWEATER_HMP // Disable HMP to get all workers queried
        auto hmp_setting{ hmp };
        hmp = false;
#       endif // PSI_SWEATER_HMP
        int result{ 2 };
        spread_the_sweat
        (
            number_of_worker_threads(),
            [ &, target_handle = thread.get_id() ]( iterations_t, iterations_t ) noexcept
            {
                auto const current{ pthread_self() };
                if ( current == target_handle )
                {
                    BOOST_ASSERT( result == 2 );
                    result = thread.bind_to_cpu( ::gettid(), mask );
                    BOOST_ASSERT( result != 2 );
                }
            }
        );
#       if PSI_SWEATER_HMP
        hmp = hmp_setting;
#       endif // PSI_SWEATER_HMP
        BOOST_ASSERT( result != 2 );
        succeeded |= ( result == 1 ); // the bool of thread::bind_to_cpu()
#       endif // EWS or spread version
#   endif // Linux
    }
    return succeeded;
}

bool shop::bind_worker_to_cpu( hardware_concurrency_t const worker_index, unsigned const cpu_id ) noexcept
{
#ifdef __EMSCRIPTEN__
    (void)worker_index; (void)cpu_id;
    return false;
#else
    cpu_affinity_mask mask;
    mask.add_cpu( cpu_id );
    return bind_worker( worker_index, mask );
#endif
}

PSI_COLD
void shop::set_max_allowed_threads( hardware_concurrency_t const max_threads )
{
    // Invitations to a spread that has already completed may still be queued
    // (see spread_with_helpers()): the workers discard them as they stop.
    BOOST_ASSERT_MSG( queue_.empty() || ( number_of_items() <= queued_helpers_.load( std::memory_order_relaxed ) ), "Cannot change parallelism level while items are in queue." );
    BOOST_ASSERT_MSG( !hmp          , "Cannot change number of workers directly when HMP is enabled" );
    stop_and_destroy_pool();
    create_pool( max_threads - PSI_SWEATER_USE_CALLER_THREAD );
}

hardware_concurrency_t shop::number_of_items() const noexcept
{
#if 0
    return queue_.depth();
#else
    return work_items_.load( std::memory_order_acquire );
#endif
}

#if PSI_SWEATER_HMP
PSI_COLD
void shop::configure_hmp( hmp_clusters_info const config, std::uint8_t const number_of_clusters )
{
    BOOST_ASSERT( number_of_clusters <= config.max_clusters );
    hmp_clusters.number_of_clusters = number_of_clusters;

    hardware_concurrency_t number_of_cores{ 0 };

    float cluster_capacities[ config.max_clusters ];
    float total_power{ 0 };
    for ( auto cluster{ 0 }; cluster < number_of_clusters; ++cluster )
    {
        hmp_clusters.cores[ cluster ] = config.cores[ cluster ];
        cluster_capacities[ cluster ] = config.power[ cluster ];
        number_of_cores += hmp_clusters.cores[ cluster ];
        total_power     += cluster_capacities[ cluster ];
    }

    std::uint8_t final_total_power{ 0 };
    for ( auto cluster{ 0 }; cluster < number_of_clusters; ++cluster )
    {
        hmp_clusters.power[ cluster ] = static_cast<std::uint8_t>( cluster_capacities[ cluster ] / total_power * hmp_config::max_power );
        final_total_power += hmp_clusters.power[ cluster ];
    }
    BOOST_ASSERT // max integer rounding error
    (
        final_total_power >= ( hmp_config::max_power - ( number_of_clusters - 1 ) ) &&
        final_total_power <= ( hmp_config::max_power + ( number_of_clusters - 1 ) )
    );
    auto const  overflow_err{ static_cast<std::uint8_t>( std::max( 0, final_total_power     - hmp_config::max_power ) ) };
    auto const underflow_err{ static_cast<std::uint8_t>( std::max( 0, hmp_config::max_power - final_total_power     ) ) };
    hmp_clusters.power[ number_of_clusters - 1 ] -=  overflow_err;
    hmp_clusters.power[ 0                      ] += underflow_err;

    final_total_power -=  overflow_err;
    final_total_power += underflow_err;
    BOOST_ASSERT( final_total_power == hmp_config::max_power );

    hmp = !thrd_lite::slow_thread_signals;

    create_pool( number_of_cores - PSI_SWEATER_USE_CALLER_THREAD );
}
#endif // PSI_SWEATER_HMP

PSI_COLD
void shop::create_pool( hardware_concurrency_t const size )
{
    BOOST_ASSERT_MSG( size <= thrd_lite::get_hardware_concurrency_max(), "Requested parallelism level not offered in hardware." );
    auto const current_size( pool_.size() );
    if ( size == current_size )
        return;
    stop_and_destroy_pool();
    brexit_.store( false, std::memory_order_relaxed );
#if PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
    pool_.resize( size );
#else
    auto p_workers( std::make_unique<worker_thread[]>( size ) );
    pool_ = { p_workers.get(), size };
#endif // !PSI_SWEATER_MAX_HARDWARE_CONCURRENCY

#   if PSI_SWEATER_EXACT_WORKER_SELECTION
    idle_ = std::make_unique<idle_word[]>( ( size + idle_group - 1U ) / idle_group ); // zeroed: workers set their own bits
#   endif // EWS
    for ( hardware_concurrency_t worker_index{ 0 }; worker_index < size; ++worker_index )
    {
#   if PSI_SWEATER_EXACT_WORKER_SELECTION
        if ( !thrd_lite::slow_thread_signals )
            pool_[ worker_index ].token_.emplace( queue_.producer_token() );
#   endif // PSI_SWEATER_EXACT_WORKER_SELECTION
        pool_[ worker_index ] = worker_loop( worker_index );
    }
#if !PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
    p_workers.release();
#endif // !PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
    std::atomic_thread_fence( std::memory_order_seq_cst );
}


PSI_COLD
void shop::stop_and_destroy_pool() noexcept
{
    brexit_.store( true, std::memory_order_relaxed );
    wake_all_workers();
    for ( auto & worker : pool_ )
        worker.join();
    // Whatever the workers left queued (they do not stop before the queue is
    // empty, this is the backstop) is run here: in particular, the
    // invitations to a spread hold a reference to it.
    {
        work_t work;
        while ( queue_.dequeue( work, consumer_token_ ) )
        {
            work();
            work_completed();
        }
    }
#if PSI_SWEATER_EXACT_WORKER_SELECTION
    idle_.reset();
#endif
#if PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
    pool_.clear();
#else
    delete[] pool_.data();
    pool_ = {};
#endif // PSI_SWEATER_MAX_HARDWARE_CONCURRENCY
}

BOOST_NOINLINE
void shop::perform_caller_work
(
    iterations_t                   const iterations,
    spread_work_template_t const &       work_part_template,
    thrd_lite::barrier           &       completion_barrier
) noexcept
{
    events::caller_work_begin( iterations );
    work_t caller_chunk{ work_part_template };
    auto & chunk_setup{ caller_chunk.target_as<spread_work_base>() };
    chunk_setup.start_iteration = 0;
    chunk_setup.  end_iteration = iterations;
    BOOST_ASSERT( chunk_setup.p_completion_barrier == &completion_barrier );
    completion_barrier.add_expected_arrival();
    work_added_untracked();
    caller_chunk        ();
    work_completed      ();
    events::caller_work_end();
}

// Every spread hands its iterations out the same way: in pieces that are
// claimed, one at a time, by whoever gets to them (see shared_spread), which is
// also what balances the load between unequal cores, background load and slow
// thread wake-ups, and what lets spreads be started concurrently and
// recursively. A piece is a fixed fraction of a participant's even share, so
// that whoever is faster, or turns up later, still finds some left: every piece
// is a claim on a counter that all the participants share, which is what limits
// how small they can get.
std::uint8_t const spread_pieces_per_participant{ 4 };

BOOST_NOINLINE
bool shop::spread_work
(
    spread_work_template_t       work_part_template,
    iterations_t           const iterations,
    iterations_t           const parallelizable_iterations_count
) noexcept
{
    if ( PSI_UNLIKELY( iterations == 0 ) ) [[ unlikely ]]
        return true;

    events::spread_begin( iterations );

    thrd_lite::barrier completion_barrier;
    work_part_template.target_as<spread_work_base>().p_completion_barrier = &completion_barrier;

    // The calling worker of a recursive spread cannot wait for parts queued
    // behind the very item it is running, and neither can anybody else count
    // on the workers being there, which is why the caller is a participant
    // like any other.
    auto const caller_is_worker{ this_threads_shop == this };
    if ( PSI_UNLIKELY( caller_is_worker ) ) [[ unlikely ]]
        events::spread_recursive_call( this_threads_worker, number_of_items() );
    else if ( auto const items_in_shop{ number_of_items() } ) [[ unlikely ]]
        events::spread_preexisting_work( items_in_shop );
    return spread_with_helpers( work_part_template, iterations, parallelizable_iterations_count, completion_barrier, caller_is_worker );
}

// A spread whose iterations are claimed, piece by piece, by whoever gets to
// them, instead of being handed out up front. The caller claims pieces like
// everybody else (it may be a worker of this pool itself, or find the pool
// busy, and so cannot count on the workers), so the
// spread completes even if no worker ever turns up, and it waits only for
// pieces that are already running. What is queued for the workers is an
// invitation to help, not a part of the work: one that is picked up after the
// last piece was claimed does nothing. Invitations outlive the call (hence the
// heap and the reference count), but the caller's work object is touched only
// between a successful claim and the arrival that follows it, both of which
// happen while the caller is still inside the call.
// An invitation is an item of the shop like any other until a worker gets to
// it: it counts in number_of_items(), and the worker that picks it up helps
// with a spread that is not its own until its pieces run out. A caller that
// waits for its own spread never does: it takes nothing from the queue.
struct alignas( thrd_lite::destructive_interference_size ) shop::shared_spread
{
    using cursor_t = std::uint64_t; // wider than iterations_t: late claims overshoot the end

    shared_spread
    (
        shop                         &       parent_shop,
        spread_work_template_t const &       work_part_template,
        iterations_t                   const iterations,
        iterations_t                   const iterations_per_piece,
        hardware_concurrency_t         const invitations
    ) noexcept
        :
        parent    { parent_shop          },
        piece     { work_part_template   },
        end       { iterations           },
        piece_size{ iterations_per_piece },
        references{ static_cast<hardware_concurrency_t>( invitations + 1 ) }
    {
        piece.target_as<spread_work_base>().p_completion_barrier = &running;
    }

    bool all_claimed() const noexcept { return next.load( std::memory_order_relaxed ) >= end; }

    void run_pieces() noexcept
    {
        // Claims are made with an arrival already announced: once the caller
        // has seen a claim fail and, after that, nobody announced, every later
        // claim fails as well.
        running.add_expected_arrival();
        for ( ; ; )
        {
            auto const start{ next.fetch_add( piece_size ) };
            if ( start >= end )
                break;
            auto   claimed{ piece };
            auto & setup  { claimed.target_as<spread_work_base>() };
            setup.start_iteration = static_cast<iterations_t>( start );
            setup.  end_iteration = static_cast<iterations_t>( std::min<cursor_t>( start + piece_size, end ) );
            running.add_expected_arrival();
            claimed(); // arrives
        }
        running.arrive();
    }

    void release() noexcept
    {
        if ( references.fetch_sub( 1, std::memory_order_acq_rel ) == 1 )
            delete this;
    }

    struct invitation
    {
        void operator()() noexcept
        {
            auto & spread{ *p_spread };
            auto & parent{ spread.parent };
            auto const still_queued{ parent.queued_helpers_.fetch_sub( 1, std::memory_order_relaxed ) - 1 };
            if ( !spread.all_claimed() )
            {
                // Pass the wake on before starting: the caller woke only one
                // of the sleeping workers, and each worker that finds something
                // left to do wakes two more, for as long as invitations are
                // waiting.
                if ( still_queued > 0 )
                    parent.wake_idle( 2, ( this_threads_shop == &parent ) ? this_threads_worker : no_worker );
                spread.run_pieces();
            }
            spread.release();
        }
        shared_spread * p_spread;
    }; // struct invitation

    shop                              & parent    ;
    spread_work_template_t              piece     ;
    cursor_t                      const end       ;
    iterations_t                  const piece_size;
    thrd_lite::barrier                  running   ; // pieces being run and claims being made
    std::atomic<hardware_concurrency_t> references;
    alignas( thrd_lite::destructive_interference_size )
    std::atomic<cursor_t>               next{ 0 } ;
}; // struct shared_spread

BOOST_NOINLINE
bool shop::spread_with_helpers
(
    spread_work_template_t const &       work_part_template,
    iterations_t                   const iterations,
    iterations_t                         parallelizable_iterations_count,
    thrd_lite::barrier           &       completion_barrier,
    bool                           const caller_is_worker
) noexcept
{
#if !PSI_SWEATER_USE_PARALLELIZATION_COST
    parallelizable_iterations_count = 1;
#endif // PSI_SWEATER_USE_PARALLELIZATION_COST

    auto const worker_threads{ static_cast<hardware_concurrency_t>( number_of_worker_threads() ) };
    auto const other_workers { static_cast<hardware_concurrency_t>( worker_threads - caller_is_worker ) };
    auto const parts         { static_cast<hardware_concurrency_t>( std::min<iterations_t>( std::max<iterations_t>( 1, iterations / parallelizable_iterations_count ), other_workers + 1U ) ) };
    // An invitation stays queued until a worker gets to it, so a caller that
    // keeps spreading while the workers stay busy must not keep adding them:
    // never more outstanding than there are workers to pick them up. The
    // limit is the shop's, not the spread's: while one spread's invitations
    // wait for busy workers, other spreads started in the meantime invite
    // fewer workers, or none and run on their caller. The room is claimed
    // with the add itself (and the excess given back) so that concurrent
    // callers cannot overbook it.
    auto const wanted_most   { static_cast<hardware_concurrency_t>( parts - 1 ) };
    auto const outstanding   { queued_helpers_.fetch_add( wanted_most, std::memory_order_relaxed ) };
    auto const room          { static_cast<hardware_concurrency_t>( std::max<std::int64_t>( 0, std::int64_t{ worker_threads } - outstanding ) ) };
    auto const wanted        { std::min<hardware_concurrency_t>( wanted_most, room ) };
    queued_helpers_.fetch_sub( wanted_most - wanted, std::memory_order_relaxed );

    shared_spread * p_spread{ nullptr };
    if ( wanted )
    {
        // A few pieces per participant, see spread_pieces_per_participant.
        auto const piece_size{ std::max<iterations_t>( 1, iterations / ( static_cast<iterations_t>( wanted + 1 ) * spread_pieces_per_participant ) ) };
        p_spread = new ( std::nothrow ) shared_spread{ *this, work_part_template, iterations, piece_size, wanted };
#   if PSI_SWEATER_USE_CALLER_THREAD
        if ( p_spread )
            p_spread->running.use_spin_wait( !caller_is_worker );
#   endif // PSI_SWEATER_USE_CALLER_THREAD
    }
    if ( !p_spread )
    {
        queued_helpers_.fetch_sub( wanted, std::memory_order_relaxed );
        perform_caller_work( iterations, work_part_template, completion_barrier );
        return true;
    }

    hardware_concurrency_t invited{ 0 };
    for ( ; invited < wanted; ++invited )
    {
        work_added_untracked();
        if ( PSI_UNLIKELY( !queue_.enqueue( shared_spread::invitation{ p_spread } ) ) )
        {
            work_completed();
            break;
        }
    }
    for ( auto unsent{ invited }; unsent < wanted; ++unsent )
    {
        queued_helpers_.fetch_sub( 1, std::memory_order_relaxed );
        p_spread->release();
    }
    if ( invited )
        wake_idle( 1, caller_is_worker ? this_threads_worker : no_worker ); // the helpers wake the rest, see shared_spread::invitation

    events::caller_work_begin( iterations );
    work_added_untracked();
    p_spread->run_pieces();
    work_completed      ();
    events::caller_work_end();

    events::caller_join_begin( !caller_is_worker );
#if PSI_SWEATER_USE_CALLER_THREAD
    if ( !caller_is_worker )
    {
        // Somebody else's thread that has nothing else to do waits by
        // spinning (a short spread is done by the time it would have fallen
        // asleep); a worker leaves its core to the ones it waits for.
        p_spread->running.spin_wait
        (
#       if PSI_SWEATER_SPIN_BEFORE_SUSPENSION
            caller_spin_count
#       endif // PSI_SWEATER_SPIN_BEFORE_SUSPENSION
        );
    }
    else
#endif // PSI_SWEATER_USE_CALLER_THREAD
    {
        p_spread->running.wait();
    }
    events::caller_join_end();

    p_spread->release();
    events::spread_end( invited, true );
    return true;
}

void shop::wake_all_workers() noexcept
{
#if PSI_SWEATER_EXACT_WORKER_SELECTION
    if ( !thrd_lite::slow_thread_signals )
    {
        for ( auto & worker : pool_ )
            worker.notify();
    }
    else
#endif
    {
#   if !PSI_SWEATER_EXACT_WORKER_SELECTION || defined( __ANDROID__ )
        work_semaphore_.signal( number_of_worker_threads() );
#   else
        BOOST_UNREACHABLE();
#   endif
    }
}

//...mrmlj...allowing underflow/overflow because of late fetch_add in fire_and_forget and concurrent invocation 'races'
void shop::work_added    ( hardware_concurrency_t const items ) noexcept
{
    work_items_.fetch_add( items, std::memory_order_acquire );
    if ( items ) { detail::in_flight_inc(); }
}
void shop::work_added_untracked( hardware_concurrency_t const items ) noexcept
{
    work_items_.fetch_add( items, std::memory_order_acquire );
}
void shop::work_completed(                                    ) noexcept { /*thrd_lite::detail::underflow_checked_dec( work_items_        );*/ work_items_.fetch_sub( 1    , std::memory_order_release ); }

#if PSI_SWEATER_EXACT_WORKER_SELECTION
shop::worker_thread & shop::next_dispatch_target() noexcept
{
    auto const workers{ number_of_worker_threads() };
    BOOST_ASSUME( workers > 0 );
    // Depth-bounded sticky dispatch: stay on the current target while its
    // backlog is small, spill to another worker only once it piles up. The
    // backlog proxy is the target's event semaphore's unconsumed-token count
    // (one signal per enqueued item; an awake worker burns leftover tokens
    // through its empty-drain wait() cycles, so the count only accumulates
    // while the worker is BUSY -- exactly the "this one needs help" signal).
    // Signaling a non-parked worker is syscall-free (the semaphore's exact
    // sleeper accounting), so a serial-ish stream of fires keeps hitting the
    // same warm, awake worker at fetch_add cost per item -- profiling showed
    // the previous unconditional round-robin (advance whenever ANY work was
    // in flight) scattered every burst across the whole pool, each item
    // waking a worker that had just drained its single item and parked
    // (~3/4 of the fire path's cost was the per-item wake syscall). Real,
    // non-trivial work fills the target's backlog past the threshold and
    // spills, progressively engaging the pool -- so parallelism is preserved
    // where it matters (items long enough to overlap) and traded for syscall
    // elision only where it does not (items shorter than the dispatch cost).
    // The stealing dequeue path remains the backstop for any imbalance.
    // The token count says nothing about the item the worker is running (it
    // consumed that token when it woke up), so a worker busy with a long item
    // shows no backlog: executing_ covers that - an item queued behind it
    // would wait for that item to finish while the rest of the pool sleeps
    // (the sleeping workers would only steal it once signalled).
    constexpr std::int32_t sticky_dispatch_depth{ 2 };
    auto const rotor { dispatch_rotor_.load( std::memory_order_relaxed ) };
    auto &     target{ pool_[ static_cast<hardware_concurrency_t>( rotor % workers ) ] };
    if ( !target.executing_.load( std::memory_order_relaxed ) && ( target.event_.pending() <= sticky_dispatch_depth ) ) [[ likely ]]
        return target;
    // Spill to a worker that waits for work, if there is one...
    for ( hardware_concurrency_t word{ 0 }; word * idle_group < workers; ++word )
    {
        auto const idle{ idle_[ word ].bits.load( std::memory_order_relaxed ) };
        if ( idle )
        {
            auto const found{ static_cast<hardware_concurrency_t>( word * idle_group + std::countr_zero( idle ) ) };
            dispatch_rotor_.store( found, std::memory_order_relaxed );
            return pool_[ found ];
        }
    }
    // ...or spread the backlog round-robin when nobody waits.
    auto const next{ dispatch_rotor_.fetch_add( 1, std::memory_order_relaxed ) + 1 };
    return pool_[ static_cast<hardware_concurrency_t>( next % workers ) ];
}

bool shop::enqueue_fire( work_t && work ) noexcept
{
    return enqueue_on( next_dispatch_target(), std::move( work ) );
}

// The target may have started running an item since it was picked, leaving
// the work queued behind it: the signal -> fence -> executing_ load half of the
// pairing with the worker loop's executing_ store -> fence -> token check, and
// then a worker that waits is woken to steal the item.
bool shop::enqueue_on( worker_thread & target, work_t && work ) noexcept
{
    auto const enqueued{ target.enqueue( std::move( work ), queue_ ) }; // ends with the signal, a read-modify-write
    fence_after_rmw();
    if ( target.executing_.load( std::memory_order_relaxed ) ) [[ unlikely ]]
        wake_idle( 1, static_cast<hardware_concurrency_t>( &target - &pool_[ 0 ] ) );
    return enqueued;
}

void shop::set_idle( hardware_concurrency_t const worker_index ) noexcept
{
    idle_[ worker_index / idle_group ].bits.fetch_or( std::uint64_t{ 1 } << ( worker_index % idle_group ), std::memory_order_seq_cst );
    fence_after_rmw();
}

void shop::withdraw_idle( hardware_concurrency_t const worker_index ) noexcept
{
    auto const bit { std::uint64_t{ 1 } << ( worker_index % idle_group ) };
    auto &     word{ idle_[ worker_index / idle_group ].bits };
    if ( word.load( std::memory_order_relaxed ) & bit ) // not claimed
        word.fetch_and( ~bit, std::memory_order_relaxed );
}

void shop::worker_thread::notify() noexcept { event_.signal(); }

bool shop::worker_thread::enqueue( work_t && __restrict work, my_queue & __restrict queue ) noexcept
{
    BOOST_ASSUME( !thrd_lite::slow_thread_signals );
    bool success;
    {
        std::scoped_lock<thrd_lite::spin_lock> const token_lock{ token_mutex_ };
        success = queue.enqueue( std::move( work ), *token_ );
    }
    notify();
    return success;
}

bool shop::worker_thread::enqueue( std::move_iterator< work_t * > const p_work, hardware_concurrency_t const number_of_items, my_queue & __restrict queue, bool const notify_worker /*= true*/ ) noexcept
{
    BOOST_ASSUME( !thrd_lite::slow_thread_signals );
    BOOST_ASSERT( number_of_items                 );
    bool success;
    {
        std::scoped_lock<thrd_lite::spin_lock> const token_lock{ token_mutex_ };
        success = queue.enqueue_bulk( *token_, p_work, number_of_items );
    }
    if ( notify_worker )
    {
        notify();
    }
    return success;
}

bool shop::claim_idle( hardware_concurrency_t const worker_index ) noexcept
{
    auto const bit{ std::uint64_t{ 1 } << ( worker_index % idle_group ) };
    auto &     word{ idle_[ worker_index / idle_group ].bits };
    if ( !( word.load( std::memory_order_relaxed ) & bit ) || !( word.fetch_and( ~bit, std::memory_order_seq_cst ) & bit ) )
        return false;
    pool_[ worker_index ].notify(); // the bit was set: it is ours to signal
    return true;
}

// Spread wake propagation: a worker woken with (spread) work still in
// flight wakes its two children in the binary wake tree over ABSOLUTE
// worker indices, rooted at worker 0 (children of i are 2i+1 / 2i+2 --
// which matches the dispatch origin, since the non-HMP dispatch path
// always starts handing out slices at worker 0). This gives log2(N) wake
// depth without the caller serially paying one wake syscall per worker on
// its critical path (the measured dominant term of small-spread joins: one
// syscall per worker, ~1-4 us each). The caller wakes only the tree root;
// propagation is a heuristic best-effort -- a chain that skips a worker
// (HMP dispatch starting mid-pool, a worker draining the queue early, a
// worker that is not idle) simply means the remaining work is done by the
// already-awake threads and the (always-participating, work-stealing)
// caller. The children are the workers whose queues the slices were handed
// to, which is why they and not just any idle workers are woken here.
void shop::propagate_spread_wake( hardware_concurrency_t const worker_index ) noexcept
{
    auto const workers{ number_of_worker_threads() };
    auto const left   { static_cast<hardware_concurrency_t>( 2 * worker_index + 1 ) };
    auto const right  { static_cast<hardware_concurrency_t>( 2 * worker_index + 2 ) };
    if ( left  < workers ) { claim_idle( left  ); }
    if ( right < workers ) { claim_idle( right ); }
}

#endif // PSI_SWEATER_EXACT_WORKER_SELECTION

hardware_concurrency_t shop::wake_idle( hardware_concurrency_t count, [[ maybe_unused ]] hardware_concurrency_t const skip ) noexcept
{
#if PSI_SWEATER_EXACT_WORKER_SELECTION
    if ( !thrd_lite::slow_thread_signals )
    {
        std::atomic_thread_fence( std::memory_order_seq_cst ); // queued work -> fence -> the set, see the idle set's pairing
        hardware_concurrency_t woken{ 0 };
        auto const words{ ( number_of_worker_threads() + idle_group - 1U ) / idle_group };
        for ( hardware_concurrency_t word_index{ 0 }; ( word_index < words ) && count; ++word_index )
        {
            auto & word{ idle_[ word_index ].bits };
            auto   idle{ word.load( std::memory_order_relaxed ) };
            if ( ( skip != no_worker ) && ( skip / idle_group == word_index ) )
                idle &= ~( std::uint64_t{ 1 } << ( skip % idle_group ) );
            while ( idle && count )
            {
                auto const bit_index{ static_cast<unsigned>( std::countr_zero( idle ) ) };
                idle &= idle - 1;
                if ( claim_idle( static_cast<hardware_concurrency_t>( word_index * idle_group + bit_index ) ) )
                {
                    ++woken;
                    --count;
                }
            }
        }
        return woken;
    }
#endif // EWS
#if !PSI_SWEATER_EXACT_WORKER_SELECTION || defined( __ANDROID__ )
    work_semaphore_.signal( count );
    return count;
#else
    BOOST_UNREACHABLE();
#endif
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif // clang

//------------------------------------------------------------------------------
} // namespace psi::sweater::generic
//------------------------------------------------------------------------------
