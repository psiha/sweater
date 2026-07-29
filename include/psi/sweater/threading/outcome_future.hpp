////////////////////////////////////////////////////////////////////////////////
///
/// \file outcome_future.hpp
/// ------------------------
///
/// A third alternative to psi::thrd_lite::future (future.hpp) and
/// std::future: the completed value is a Boost.Outcome-family outcome<T>
/// (value / std::error_code / std::exception_ptr) instead of a value the
/// consumer retrieves via a throwing get(). outcome_future<T>::get() itself
/// is noexcept -- the consumer checks has_value()/has_exception() (or
/// error()) on the returned outcome<T> instead of wrapping get() in a
/// try/catch. The producer side still catches whatever the scheduled work
/// throws (outcome_promise::run()) -- this changes who is FORCED to deal
/// with exceptions at the API boundary, not whether exceptions exist in the
/// implementation.
///
/// Peer to thrd_lite::future, not a replacement: reuses the same
/// futex-backed psi::thrd_lite::semaphore for completion signaling and the
/// same refcounted (exactly two owners) slot lifetime -- see future.hpp's
/// file-level note for why single ownership is unsound here (semaphore::
/// signal() keeps touching its own memory after the point where a paired
/// wait() call can already unblock and return to its caller).
///
/// Only compiled when PSI_SWEATER_WITH_OUTCOME=ON at CMake configure time
/// (sweater.cmake) -- opt-in, since it pulls in an Outcome dependency
/// (standalone ned14/outcome via CPM by default, or an existing Boost::boost
/// target's boost/outcome.hpp with PSI_SWEATER_OUTCOME_STANDALONE=OFF; see
/// sweater.cmake's Outcome block).
////////////////////////////////////////////////////////////////////////////////
#pragma once
//------------------------------------------------------------------------------
#ifndef PSI_SWEATER_HAS_OUTCOME
#error "psi/sweater/threading/outcome_future.hpp requires PSI_SWEATER_WITH_OUTCOME=ON at CMake configure time (see sweater.cmake)"
#endif
//------------------------------------------------------------------------------
#if PSI_SWEATER_OUTCOME_STANDALONE
#include <outcome.hpp>
#else
#include <boost/outcome.hpp>
#endif

#include "../detail/config.hpp"
#include "future.hpp" // broken_promise
#include "semaphore.hpp"

#include <boost/assert.hpp>
#include <boost/core/no_exceptions_support.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <new>
#include <type_traits>
#include <utility>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

namespace detail
{
#if PSI_SWEATER_OUTCOME_STANDALONE
    namespace outcome_ns = OUTCOME_V2_NAMESPACE;
#else
    namespace outcome_ns = BOOST_OUTCOME_V2_NAMESPACE;
#endif
} // namespace detail

/// outcome<T>: value, OR std::error_code, OR std::exception_ptr (all three
/// slots coexist -- unlike result<T>, which is strictly value-or-one-error).
/// This is what run()'s caught exceptions and any future error-code-
/// returning work land in.
template <typename T>
using outcome_result = detail::outcome_ns::outcome<T>;

namespace detail
{
    template <typename T>
    class outcome_state
    {
    public:
        outcome_state() noexcept = default;

        outcome_state( outcome_state const & ) = delete;
        outcome_state & operator=( outcome_state const & ) = delete;

        template <typename ... Args>
        void set( Args && ... args ) noexcept( std::is_nothrow_constructible_v<outcome_result<T>, Args && ...> )
        {
            BOOST_ASSERT( !completed_ );
            ::new ( static_cast<void *>( &storage_ ) ) outcome_result<T>( std::forward<Args>( args ) ... );
            completed_ = true;
            completion_.signal();
        }

        [[ nodiscard ]] bool completed() const noexcept { return completed_; }

        void wait() noexcept
        {
            if ( !waited_ )
            {
                completion_.wait();
                waited_ = true;
            }
        }

        outcome_result<T> get() noexcept
        {
            wait();
            BOOST_ASSERT( completed_ );
            return std::move( *reinterpret_cast<outcome_result<T> *>( &storage_ ) );
        }

        // Exactly two owners (promise + future) -- see future.hpp's
        // file-level note for why this has to be refcounted.
        void release() noexcept
        {
            if ( owners_.fetch_sub( 1, std::memory_order_acq_rel ) == 1 )
                delete this;
        }

    private:
        ~outcome_state() noexcept
        {
            if ( completed_ )
                reinterpret_cast<outcome_result<T> *>( &storage_ )->~outcome_result<T>();
        }

        semaphore                                    completion_;
        alignas( outcome_result<T> ) std::byte        storage_[ sizeof( outcome_result<T> ) ];
        std::atomic<std::uint8_t>                     owners_   { 2 }; // fixed at exactly 2 -- packs with the two bools below
        bool                                          completed_{ false };
        bool                                          waited_   { false };
    }; // class outcome_state
} // namespace detail

template <typename T> class outcome_future;

/// Producer side. Move-only.
template <typename T>
class outcome_promise
{
public:
    outcome_promise() noexcept = default;

    outcome_promise( outcome_promise && other ) noexcept : p_state_{ other.p_state_ } { other.p_state_ = nullptr; }
    outcome_promise & operator=( outcome_promise && other ) noexcept
    {
        abandon();
        p_state_ = other.p_state_;
        other.p_state_ = nullptr;
        return *this;
    }
    outcome_promise( outcome_promise const & ) = delete;
    outcome_promise & operator=( outcome_promise const & ) = delete;

    ~outcome_promise() noexcept { abandon(); }

    template <typename ... Args>
    void set( Args && ... args ) noexcept( noexcept( std::declval<detail::outcome_state<T> &>().set( std::forward<Args>( args ) ... ) ) )
    {
        BOOST_ASSERT( p_state_ );
        p_state_->set( std::forward<Args>( args ) ... );
    }

    /// Invoke work and route its result (or caught exception) into the
    /// paired outcome_future. Never throws.
    template <typename F>
    void run( F && work ) noexcept
    {
        BOOST_ASSERT( p_state_ );
        BOOST_TRY
        {
            if constexpr ( std::is_void_v<T> )
            {
                std::forward<F>( work )();
                p_state_->set( detail::outcome_ns::success() );
            }
            else
            {
                p_state_->set( outcome_result<T>{ std::forward<F>( work )() } );
            }
        }
        BOOST_CATCH( ... )
        {
            p_state_->set( outcome_result<T>{ std::current_exception() } );
        }
        BOOST_CATCH_END
    }

private:
    friend class outcome_future<T>;

    explicit outcome_promise( detail::outcome_state<T> & state ) noexcept : p_state_{ &state } {}

    // Dropped without ever completing -- fail the future instead of leaving
    // its wait()/get() blocked forever (same reasoning as future.hpp's
    // promise::abandon(), adapted to land in the outcome's exception slot).
    void abandon() noexcept
    {
        if ( p_state_ )
        {
            if ( PSI_UNLIKELY( !p_state_->completed() ) )
                p_state_->set( outcome_result<T>{ std::make_exception_ptr( broken_promise{} ) } );
            p_state_->release();
        }
    }

    detail::outcome_state<T> * p_state_{ nullptr };
}; // class outcome_promise

/// Consumer side. Move-only. wait()/get() are each idempotent but, like
/// thrd_lite::future/std::future, must only ever be called from a single
/// thread. get() is noexcept -- unlike thrd_lite::future<T>::get(), it never
/// throws; the returned outcome_result<T> carries the exception instead.
template <typename T>
class outcome_future
{
public:
    outcome_future() noexcept = default;

    outcome_future( outcome_future && other ) noexcept : p_state_{ other.p_state_ } { other.p_state_ = nullptr; }
    outcome_future & operator=( outcome_future && other ) noexcept
    {
        release();
        p_state_ = other.p_state_;
        other.p_state_ = nullptr;
        return *this;
    }
    outcome_future( outcome_future const & ) = delete;
    outcome_future & operator=( outcome_future const & ) = delete;

    ~outcome_future() noexcept { release(); }

    [[ nodiscard ]] bool valid() const noexcept { return p_state_ != nullptr; }

    void wait() noexcept { BOOST_ASSERT( p_state_ ); p_state_->wait(); }

    [[ nodiscard ]] outcome_result<T> get() noexcept { BOOST_ASSERT( p_state_ ); return p_state_->get(); }

    [[ nodiscard ]] static std::pair<outcome_promise<T>, outcome_future<T>> make()
    {
        auto & state{ *new detail::outcome_state<T>() }; // owners_ starts at 2 (promise + future)
        outcome_promise<T> prom{ state };
        return { std::move( prom ), outcome_future{ state } };
    }

private:
    explicit outcome_future( detail::outcome_state<T> & state ) noexcept : p_state_{ &state } {}

    void release() noexcept
    {
        if ( p_state_ )
        {
            p_state_->wait();
            p_state_->release();
            p_state_ = nullptr;
        }
    }

    detail::outcome_state<T> * p_state_{ nullptr };
}; // class outcome_future

/// Constructs a single-use promise/future pair sharing one heap-allocated
/// slot (refcounted -- see the file-level note).
template <typename T>
[[ nodiscard ]] std::pair<outcome_promise<T>, outcome_future<T>> make_outcome_promise_future() { return outcome_future<T>::make(); }

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
