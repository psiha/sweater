cmake_minimum_required( VERSION 3.27.0 )

if ( NOT PROJECT_IS_TOP_LEVEL )
    return()
endif()

if ( CMAKE_CXX_COMPILER_ID MATCHES "Clang" )
    # googletest 1.15 trips clang 21's new -Wcharacter-conversion (promoted to an error
    # by the config_ex -Werror defaults); directory-scoped so it covers the gtest build
    # and the gtest headers included by the tests below.
    add_compile_options( -Wno-character-conversion )
endif()

CPMAddPackage( "gh:google/googletest@1.15.2" )
include( GoogleTest )

function( sweater_add_test target_name source )
    add_executable( ${target_name} ${source} )
    target_link_libraries( ${target_name} PRIVATE GTest::gtest_main psi::sweater )
    set_target_properties( ${target_name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test" )
    add_test( NAME ${target_name} COMMAND ${target_name} )
endfunction()

sweater_add_test( sweater_smoke_test     smoke_test.cpp     )

# Consolidated TYPED_TEST_SUITE coverage for rw_mutex/futex_rw_mutex and their
# reader/writer-preferring variants -- supersedes the former sweater_rw_mutex_test
# and sweater_rrw_mutex_test targets (see rw_mutex_contract_test.cpp's header
# comment). mutex_test_traits.hpp is a header-only test helper, not a target.
sweater_add_test( sweater_rw_mutex_contract_test rw_mutex_contract_test.cpp )

# Type-identity checks only (see rw_preference_test.cpp's header comment); compiles to
# zero tests where reader_preferring_rw_mutex is not the glibc NP-rwlock-kind mechanism
# (Windows SRWLOCK, macOS pthread_rwlock -- neither offers a kind-setting API), see
# posix/rw_mutex.hpp. That is expected, not a gap -- the behavioral coverage for
# reader_preferring_rw_mutex on those platforms lives in rw_mutex_contract_test.cpp.
add_executable( sweater_rw_preference_test rw_preference_test.cpp )
target_link_libraries( sweater_rw_preference_test PRIVATE GTest::gtest_main psi::sweater )
set_target_properties( sweater_rw_preference_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test" )
add_test( NAME sweater_rw_preference_test COMMAND sweater_rw_preference_test )

# Low-level psi::thrd_lite::futex tests (bitset wait/wake targeting) -- same backend
# availability as futex_rw_mutex (see rw_mutex_contract_test.cpp for its behavioral
# coverage, folded in above).
sweater_add_test( sweater_futex_test futex_test.cpp )

# Libuv backend tests — only when libuv headers + library are available.
find_path( SWEATER_LIBUV_INCLUDE_DIR NAMES uv.h )
find_library( SWEATER_LIBUV_LIBRARY NAMES uv libuv )

if ( SWEATER_LIBUV_INCLUDE_DIR AND SWEATER_LIBUV_LIBRARY )
    message( STATUS "sweater: libuv found — building sweater_libuv_test (${SWEATER_LIBUV_LIBRARY})" )

    add_executable( sweater_libuv_test libuv_test.cpp )
    target_include_directories( sweater_libuv_test PRIVATE "${SWEATER_LIBUV_INCLUDE_DIR}" )
    target_link_libraries( sweater_libuv_test PRIVATE GTest::gtest_main psi::sweater "${SWEATER_LIBUV_LIBRARY}" )
    set_target_properties( sweater_libuv_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test" )
    add_test( NAME sweater_libuv_test COMMAND sweater_libuv_test )
else()
    message( STATUS "sweater: libuv not found — skipping sweater_libuv_test" )
endif()
