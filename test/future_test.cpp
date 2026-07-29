//==============================================================================
// Tests for the standalone psi::thrd_lite::promise/future pair (future.hpp) --
// independent of any sweater shop/dispatch() usage. The contract itself
// (shared with outcome_future_test.cpp) lives in future_contract_test.hpp as
// a gtest type-parameterized suite; this file just supplies the Kind and
// instantiates it.
//==============================================================================

#include "future_contract_test.hpp"

#include <psi/sweater/threading/future.hpp>

#include <exception>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

struct ThrdLiteFutureKind
{
    template <typename T> using future_type  = future<T>;
    template <typename T> using promise_type = promise<T>;

    template <typename T>
    static std::pair<promise_type<T>, future_type<T>> make() { return make_promise_future<T>(); }

    template <typename T>
    static T get_value( future_type<T> & f ) { return f.get(); } // throws on a failure state

    template <typename T>
    static void expect_exception( future_type<T> & f )
    {
        EXPECT_THROW( f.get(), std::exception ); // broken_promise / std::runtime_error both derive from it
    }
}; // struct ThrdLiteFutureKind

INSTANTIATE_TYPED_TEST_SUITE_P( ThrdLite, FutureContract, ThrdLiteFutureKind );

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
