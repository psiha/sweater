cmake_minimum_required( VERSION 3.27.0 )

if ( NOT PROJECT_IS_TOP_LEVEL )
    return()
endif()

CPMAddPackage( "gh:google/googletest@1.15.2" )
include( GoogleTest )

function( sweater_add_smoke_test target_name )
    add_executable( ${target_name} smoke_test.cpp )
    target_link_libraries( ${target_name} PRIVATE GTest::gtest_main psi::sweater )
    set_target_properties( ${target_name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test" )
    add_test( NAME ${target_name} COMMAND ${target_name} )
endfunction()

sweater_add_smoke_test( sweater_smoke_test )

add_executable( sweater_rrw_mutex_test rrw_mutex_test.cpp )
target_link_libraries( sweater_rrw_mutex_test PRIVATE GTest::gtest_main psi::sweater )
set_target_properties( sweater_rrw_mutex_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test" )
add_test( NAME sweater_rrw_mutex_test COMMAND sweater_rrw_mutex_test )

# Portable across backends: skips itself (GTEST_SKIP) where reader_preferring_rw_mutex
# is not defined (Windows SRWLOCK, macOS pthread_rwlock -- neither offers an NP
# kind-setting API), see posix/rw_mutex.hpp.
add_executable( sweater_rw_preference_test rw_preference_test.cpp )
target_link_libraries( sweater_rw_preference_test PRIVATE GTest::gtest_main psi::sweater )
set_target_properties( sweater_rw_preference_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test" )
add_test( NAME sweater_rw_preference_test COMMAND sweater_rw_preference_test )

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
