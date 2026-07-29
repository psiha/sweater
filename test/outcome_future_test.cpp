//==============================================================================
// Tests for psi::thrd_lite::outcome_promise/outcome_future (outcome_future.hpp)
// -- the Outcome-based peer to thrd_lite::promise/future. The shared contract
// (with future_test.cpp) lives in future_contract_test.hpp as a gtest
// type-parameterized suite; this file supplies the Kind, instantiates it, and
// adds the one invariant unique to this Kind (get() never throwing). Only
// built when PSI_SWEATER_WITH_OUTCOME=ON (see test/CMakeLists.txt).
//==============================================================================

#include "future_contract_test.hpp"

#include <psi/sweater/threading/outcome_future.hpp>

#include <stdexcept>
#include <type_traits>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

struct OutcomeFutureKind
{
    template <typename T> using future_type  = outcome_future<T>;
    template <typename T> using promise_type = outcome_promise<T>;

    template <typename T>
    static std::pair<promise_type<T>, future_type<T>> make() { return make_outcome_promise_future<T>(); }

    template <typename T>
    static T get_value( future_type<T> & f )
    {
        auto result( f.get() );
        EXPECT_TRUE( result.has_value() );
        if constexpr ( !std::is_void_v<T> )
            return std::move( result ).value();
    }

    template <typename T>
    static void expect_exception( future_type<T> & f )
    {
        EXPECT_TRUE( f.get().has_exception() );
    }
}; // struct OutcomeFutureKind

INSTANTIATE_TYPED_TEST_SUITE_P( Outcome, FutureContract, OutcomeFutureKind );

// Unique to this Kind -- thrd_lite::future<T>::get() throws by design, so
// this isn't part of the shared FutureContract suite.
TEST( ThrdLiteOutcomeFutureOnly, GetNeverThrows )
{
    auto pair( make_outcome_promise_future<int>() );
    pair.first.run( []() -> int { throw std::runtime_error{ "boom" }; } );
    EXPECT_NO_THROW( { auto const result( pair.second.get() ); (void)result; } );
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
