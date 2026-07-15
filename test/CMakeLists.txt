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
sweater_add_test( sweater_rw_mutex_test  rw_mutex_test.cpp  )
sweater_add_test( sweater_rrw_mutex_test rrw_mutex_test.cpp )

# Portable across backends: skips itself (GTEST_SKIP) where reader_preferring_rw_mutex
# is not defined (Windows SRWLOCK, macOS pthread_rwlock -- neither offers an NP
# kind-setting API), see posix/rw_mutex.hpp.
add_executable( sweater_rw_preference_test rw_preference_test.cpp )
target_link_libraries( sweater_rw_preference_test PRIVATE GTest::gtest_main psi::sweater )
set_target_properties( sweater_rw_preference_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test" )
add_test( NAME sweater_rw_preference_test COMMAND sweater_rw_preference_test )

# Low-level psi::thrd_lite::futex tests (bitset wait/wake targeting) -- same backend
# availability as the futex_rw_mutex test below.
sweater_add_test( sweater_futex_test futex_test.cpp )

# Experimental futex-based rwlock -- available wherever psi::thrd_lite::futex has a
# real backend: Linux (SYS_futex), Windows (WaitOnAddress, cross-validation only),
# and Apple (__ulock_wait/__ulock_wake, PRIVATE API -- see apple/futex.cpp and
# futex_rw_mutex.hpp's design-doc comment for why that's accepted only here, not
# on any production path).
sweater_add_test( sweater_futex_rw_mutex_test futex_rw_mutex_test.cpp )

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
