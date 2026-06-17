cmake_minimum_required( VERSION 3.27.0 )

if ( NOT PROJECT_IS_TOP_LEVEL )
    return()
endif()

CPMAddPackage( "gh:google/googletest@1.15.2" )
include( GoogleTest )

add_executable( sweater_smoke_test smoke_test.cpp )
target_link_libraries( sweater_smoke_test PRIVATE GTest::gtest_main psi::sweater )

set_target_properties( sweater_smoke_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test" )
add_test( NAME sweater_smoke_test COMMAND sweater_smoke_test )
