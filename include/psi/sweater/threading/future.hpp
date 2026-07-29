////////////////////////////////////////////////////////////////////////////////
///
/// \file future.hpp
/// ----------------
///
/// A one-shot promise/future pair, the thrd_lite counterpart to
/// std::promise/std::future: completion is signaled through
/// psi::thrd_lite::semaphore (the futex-backed primitive already used for
/// worker wake-up elsewhere in this library) instead of std::promise's own
/// internally heap-allocated shared state plus mutex/condvar.
///
/// The shared slot IS refcounted -- exactly two owners, promise and future,
/// each releasing on destruction, the last one deleting it. This is not
/// incidental: psi::thrd_lite::semaphore::signal() keeps touching its own
/// memory (credits_, sleepers_ -- see futex_semaphore.cpp's design-doc
/// comment) *after* the point where a waiting wait() call can already
/// unblock and return to its caller, so a future that simply deleted the
/// slot the instant its own wait() returned could free memory a concurrent
/// signal() call was still reading -- exactly the reason std::promise/
/// std::future share ownership of their state too. The refcount here is a
/// single intrusive atomic<int> rather than a shared_ptr control block --
/// no weak_ptr, no type erasure, fixed at exactly two parties -- so this
/// stays meaningfully leaner than std::promise/std::future while remaining
/// correct.
///
/// Not thread-safe against concurrent use of the *same* promise or future
/// object from multiple threads -- exactly one thread drives each side, same
/// restriction as std::promise/std::future.
////////////////////////////////////////////////////////////////////////////////
#pragma once
//------------------------------------------------------------------------------
#include "../detail/config.hpp"
#include "semaphore.hpp"

#include <boost/assert.hpp>
#include <boost/core/no_exceptions_support.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <type_traits>
#include <utility>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

/// Thrown by future::get()/wait() when the paired promise was destroyed
/// without ever calling set_value()/set_exception()/run() -- the thrd_lite
/// counterpart to std::future_error( std::future_errc::broken_promise ),
/// without pulling in <future> just for that one error type.
class broken_promise : public std::exception
{
public:
    [[ nodiscard ]] char const * what() const noexcept override { return "psi::thrd_lite: broken promise"; }
}; // class broken_promise

namespace detail
{
    template <typename T>
    class future_state
    {
    public:
        future_state() noexcept = default;

        future_state( future_state const & ) = delete;
        future_state & operator=( future_state const & ) = delete;

        template <typename ... Args>
        void set_value( Args && ... args ) noexcept( std::is_nothrow_constructible_v<T, Args && ...> )
        {
            BOOST_ASSERT( !completed_ );
            if constexpr ( !std::is_void_v<T> )
                ::new ( static_cast<void *>( &storage_ ) ) T( std::forward<Args>( args ) ... );
            has_value_ = true;
            completed_ = true;
            completion_.signal();
        }

        void set_exception( std::exception_ptr p ) noexcept
        {
            BOOST_ASSERT( !completed_ );
            exception_ = std::move( p );
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

        T get()
        {
            wait();
            if ( PSI_UNLIKELY( exception_ ) )
                std::rethrow_exception( exception_ );
            BOOST_ASSERT( has_value_ );
            if constexpr ( !std::is_void_v<T> )
                return std::move( *reinterpret_cast<T *>( &storage_ ) );
        }

        // Exactly two owners (promise + future), each releasing exactly once
        // on their own destruction -- see the file-level note for why this
        // has to be refcounted rather than singly owned by the future.
        void release() noexcept
        {
            if ( owners_.fetch_sub( 1, std::memory_order_acq_rel ) == 1 )
                delete this;
        }

    private:
        using stored_t = std::conditional_t<std::is_void_v<T>, std::byte, T>;

        ~future_state() noexcept
        {
            if constexpr ( !std::is_void_v<T> )
            {
                if ( has_value_ )
                    reinterpret_cast<T *>( &storage_ )->~T();
            }
        }

        semaphore                       completion_;
        std::exception_ptr              exception_;
        alignas( stored_t ) std::byte   storage_[ sizeof( stored_t ) ];
        std::atomic<std::uint8_t>       owners_   { 2 }; // fixed at exactly 2 (promise + future) -- packs with the bools below
        bool                            has_value_{ false };
        bool                            completed_{ false };
        bool                            waited_   { false };
    }; // class future_state
} // namespace detail

template <typename T> class future;

/// Producer side. Move-only.
template <typename T>
class promise
{
public:
    promise() noexcept = default;

    promise( promise && other ) noexcept : p_state_{ other.p_state_ } { other.p_state_ = nullptr; }
    promise & operator=( promise && other ) noexcept
    {
        abandon();
        p_state_ = other.p_state_;
        other.p_state_ = nullptr;
        return *this;
    }
    promise( promise const & ) = delete;
    promise & operator=( promise const & ) = delete;

    ~promise() noexcept { abandon(); }

    template <typename ... Args>
    void set_value( Args && ... args ) noexcept( noexcept( std::declval<detail::future_state<T> &>().set_value( std::forward<Args>( args ) ... ) ) )
    {
        BOOST_ASSERT( p_state_ );
        p_state_->set_value( std::forward<Args>( args ) ... );
    }

    void set_exception( std::exception_ptr p ) noexcept
    {
        BOOST_ASSERT( p_state_ );
        p_state_->set_exception( std::move( p ) );
    }

    /// Convenience for the fire-and-forget-worker pattern: invoke work and
    /// route its result or exception into the paired future. Never throws --
    /// work's exception is captured, not propagated (the same contract every
    /// sweater dispatch()/fire_and_forget backend already requires of the
    /// work it schedules).
    template <typename F>
    void run( F && work ) noexcept
    {
        BOOST_ASSERT( p_state_ );
        BOOST_TRY
        {
            if constexpr ( std::is_void_v<T> )
            {
                std::forward<F>( work )();
                p_state_->set_value();
            }
            else
            {
                p_state_->set_value( std::forward<F>( work )() );
            }
        }
        BOOST_CATCH( ... )
        {
            p_state_->set_exception( std::current_exception() );
        }
        BOOST_CATCH_END
    }

private:
    friend class future<T>;

    explicit promise( detail::future_state<T> & state ) noexcept : p_state_{ &state } {}

    // Dropped without ever completing (dispatch_lite() itself never does
    // this -- every backend guarantees run() executes, or fails the future
    // explicitly; this is a safety net for direct make_promise_future()
    // misuse) -- fail the future instead of leaving its wait()/get() blocked
    // forever.
    void abandon() noexcept
    {
        if ( p_state_ )
        {
            if ( PSI_UNLIKELY( !p_state_->completed() ) )
                p_state_->set_exception( std::make_exception_ptr( broken_promise{} ) );
            p_state_->release();
        }
    }

    detail::future_state<T> * p_state_{ nullptr };
}; // class promise

/// Consumer side. Move-only. wait()/get() are each idempotent but, like
/// std::future, must only ever be called from a single thread.
template <typename T>
class future
{
public:
    future() noexcept = default;

    future( future && other ) noexcept : p_state_{ other.p_state_ } { other.p_state_ = nullptr; }
    future & operator=( future && other ) noexcept
    {
        release();
        p_state_ = other.p_state_;
        other.p_state_ = nullptr;
        return *this;
    }
    future( future const & ) = delete;
    future & operator=( future const & ) = delete;

    // Unlike std::future::~future() (which only blocks for the rare
    // std::async( launch::async ) case): this ALWAYS waits for completion
    // before releasing its share of the slot -- see the file-level note for
    // why release (not deletion outright) is what happens here.
    ~future() noexcept { release(); }

    [[ nodiscard ]] bool valid() const noexcept { return p_state_ != nullptr; }

    void wait() noexcept { BOOST_ASSERT( p_state_ ); p_state_->wait(); }

    T get() { BOOST_ASSERT( p_state_ ); return p_state_->get(); }

    [[ nodiscard ]] static std::pair<promise<T>, future<T>> make()
    {
        auto & state{ *new detail::future_state<T>() }; // owners_ starts at 2 (promise + future)
        promise<T> prom{ state };
        return { std::move( prom ), future{ state } };
    }

private:
    explicit future( detail::future_state<T> & state ) noexcept : p_state_{ &state } {}

    void release() noexcept
    {
        if ( p_state_ )
        {
            p_state_->wait();
            p_state_->release();
            p_state_ = nullptr;
        }
    }

    detail::future_state<T> * p_state_{ nullptr };
}; // class future

/// Constructs a single-use promise/future pair sharing one heap-allocated
/// slot (refcounted -- see the file-level note).
template <typename T>
[[ nodiscard ]] std::pair<promise<T>, future<T>> make_promise_future() { return future<T>::make(); }

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
