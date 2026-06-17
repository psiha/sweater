cmake_minimum_required( VERSION 3.27.0 )

project( sweater CXX )

###################
## Deps
###################

# Download CPM.cmake

file(
  DOWNLOAD
  https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/get_cpm.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/cmake/get_cpm.cmake
)
set( CPM_SOURCE_CACHE "${CMAKE_CURRENT_BINARY_DIR}/deps" )

if ( APPLE ) # :wat: GH CI
    set( GIT_EXECUTABLE /usr/bin/git CACHE FILEPATH "Path to git executable" FORCE )
endif()

include( ${CMAKE_CURRENT_BINARY_DIR}/cmake/get_cpm.cmake )


# Boost (header-only consumers: config, assert, container, core, preprocessor
# and their transitive dependencies).

set( boost_ver boost-1.90.0 )
CPMAddPackage( "gh:boostorg/static_assert#${boost_ver}"   ) # Boost::core dependency
CPMAddPackage( "gh:boostorg/throw_exception#${boost_ver}" ) # Boost::core dependency
CPMAddPackage( "gh:boostorg/config#${boost_ver}"          )
CPMAddPackage( "gh:boostorg/intrusive#${boost_ver}"       ) # Boost::container dependency
CPMAddPackage( "gh:boostorg/move#${boost_ver}"            ) # Boost::container dependency
CPMAddPackage( "gh:boostorg/type_traits#${boost_ver}"     )
CPMAddPackage( "gh:boostorg/assert#${boost_ver}"          )
CPMAddPackage( "gh:boostorg/container#${boost_ver}"       )
CPMAddPackage( "gh:boostorg/core#${boost_ver}"            )
CPMAddPackage( "gh:boostorg/preprocessor#${boost_ver}"    )

# Unofficial psiha Boost bits.
CPMAddPackage( "gh:psiha/config_ex#master"   ) # BOOST_ASSUME / BOOST_NOINLINE / ...
CPMAddPackage( "gh:psiha/functionoid#master" ) # generic-impl work_t backend (sweater.cmake picks up functionoid_SOURCE_DIR)

# An aggregate Boost::boost INTERFACE target so sweater.cmake's `if ( TARGET
# Boost::boost )` linkage resolves the same way it does inside the rama host.
if ( NOT TARGET Boost::boost )
    add_library( Boost::boost INTERFACE IMPORTED )
    target_link_libraries( Boost::boost INTERFACE
        Boost::config Boost::assert Boost::container Boost::core Boost::preprocessor
    )
    target_include_directories( Boost::boost INTERFACE "${config_ex_SOURCE_DIR}/include" )
endif()


###################
## Toolchain
###################

set( CMAKE_CXX_STANDARD 26 )
add_compile_definitions( BOOST_ALL_NO_LIB )
if ( WIN32 )
    add_compile_definitions( WIN32_LEAN_AND_MEAN NOMINMAX )
endif()


###################
## Target
###################

include( ${CMAKE_CURRENT_LIST_DIR}/sweater.cmake )


###################
## Testing (standalone)
###################

if ( PROJECT_IS_TOP_LEVEL )
    enable_testing()
    add_subdirectory( test )
endif()
