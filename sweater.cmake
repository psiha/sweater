#############################################################################
# (c) Copyright Domagoj Saric 2021 - 2024.
#
#  Use, modification and distribution are subject to the
#  Boost Software License, Version 1.0. (See accompanying file
#  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
#  See http://www.boost.org for most recent version.
#############################################################################

set( src_root "${CMAKE_CURRENT_LIST_DIR}/include/psi/sweater" )

set( sweater_sources
    ${src_root}/detail/config.hpp
    ${src_root}/dispatch_tracking.hpp
    ${src_root}/spread_chunked.cpp
    ${src_root}/spread_chunked.hpp
    ${src_root}/sweater.hpp
)

set( sources_impls
    ${src_root}/impls/apple.hpp
    ${src_root}/impls/generic.cpp
    ${src_root}/impls/generic.hpp
    ${src_root}/impls/generic_config.hpp
    ${src_root}/impls/libuv.hpp
    ${src_root}/impls/openmp.hpp
    ${src_root}/impls/single_threaded.hpp
    ${src_root}/impls/windows.hpp
)
source_group( "Impls" FILES ${sources_impls} )
list( APPEND sweater_sources ${sources_impls} )
if ( WIN32 OR APPLE )
    # Windows and Apple use native impls (windows.hpp / apple.hpp); the generic
    # thread pool's .cpp is excluded from compilation on those platforms.
    set_source_files_properties( ${src_root}/impls/generic.cpp PROPERTIES HEADER_FILE_ONLY ON )
endif()

set( sources_queues
    ${src_root}/queues/mpmc_moodycamel.hpp
)
source_group( "Queues" FILES ${sources_queues} )
list( APPEND sweater_sources ${sources_queues} )

set( sources_threading
    ${src_root}/threading/barrier.hpp
    ${src_root}/threading/futex.hpp
    ${src_root}/threading/futex_barrier.cpp
    ${src_root}/threading/futex_barrier.hpp
    ${src_root}/threading/futex_semaphore.cpp
    ${src_root}/threading/generic_barrier.cpp
    ${src_root}/threading/generic_barrier.hpp
    ${src_root}/threading/generic_semaphore.cpp
    ${src_root}/threading/hardware_concurrency.cpp
    ${src_root}/threading/hardware_concurrency.hpp
    ${src_root}/threading/condvar.hpp
    ${src_root}/threading/mutex.hpp
    ${src_root}/threading/rw_mutex.hpp
    ${src_root}/threading/semaphore.hpp
    ${src_root}/threading/thread.hpp
)
source_group( "ThrdLite" FILES ${sources_threading} )
list( APPEND sweater_sources ${sources_threading} )

if ( APPLE )
    set_source_files_properties( ${src_root}/threading/futex_barrier.cpp   ${src_root}/threading/futex_semaphore.cpp   PROPERTIES HEADER_FILE_ONLY ON )
else()
    set_source_files_properties( ${src_root}/threading/generic_barrier.cpp ${src_root}/threading/generic_semaphore.cpp PROPERTIES HEADER_FILE_ONLY ON )
endif()


set( sources_threading_cpp
    ${src_root}/threading/cpp/spin_lock.cpp
    ${src_root}/threading/cpp/spin_lock.hpp
)
source_group( "ThrdLite/Cpp" FILES ${sources_threading_cpp} )
list( APPEND sweater_sources ${sources_threading_cpp} )


set( sources_threading_emscripten
    ${src_root}/threading/emscripten/futex.cpp
)
source_group( "ThrdLite/Emscripten" FILES ${sources_threading_emscripten} )
list( APPEND sweater_sources ${sources_threading_emscripten} )
if ( NOT EMSCRIPTEN )
    set_source_files_properties( ${sources_threading_emscripten} PROPERTIES HEADER_FILE_ONLY ON )
endif()


set( sources_threading_linux
    ${src_root}/threading/linux/futex.cpp
)
source_group( "ThrdLite/Linux" FILES ${sources_threading_linux} )
list( APPEND sweater_sources ${sources_threading_linux} )
if ( NOT ANDROID AND NOT LINUX )
    set_source_files_properties( ${sources_threading_linux} PROPERTIES HEADER_FILE_ONLY ON )
endif()


set( sources_threading_posix
    ${src_root}/threading/posix/condvar.hpp
    ${src_root}/threading/posix/mutex.hpp
    ${src_root}/threading/posix/rw_mutex.hpp
    ${src_root}/threading/posix/semaphore.hpp
    ${src_root}/threading/posix/thread.cpp
    ${src_root}/threading/posix/thread.hpp
)
source_group( "ThrdLite/POSIX" FILES ${sources_threading_posix} )
list( APPEND sweater_sources ${sources_threading_posix} )
if ( WIN32 )
    set_source_files_properties( ${sources_threading_posix} PROPERTIES HEADER_FILE_ONLY ON )
endif()


set( sources_threading_windows
    ${src_root}/threading/windows/condvar.hpp
    ${src_root}/threading/windows/futex.cpp
    ${src_root}/threading/windows/mutex.hpp
    ${src_root}/threading/windows/rw_mutex.hpp
    ${src_root}/threading/windows/thread.hpp
)
source_group( "ThrdLite/Windows" FILES ${sources_threading_windows} )
list( APPEND sweater_sources ${sources_threading_windows} )
if ( NOT WIN32 )
    set_source_files_properties( ${sources_threading_windows} PROPERTIES HEADER_FILE_ONLY ON )
endif()


#############################################################################
## Target
#############################################################################
#
# Windows and Apple select native thread-pool implementations (windows.hpp /
# apple.hpp) so the generic implementation's TUs are excluded there; every
# other platform compiles the `generic` implementation (shop ctor/dtor/worker
# loop plus the futex/thread/barrier/semaphore backends). The per-file
# HEADER_FILE_ONLY properties set above exclude the wrong-platform TUs from
# the compile; the platform-independent support TUs (hardware_concurrency,
# chunked-spread arithmetic) compile everywhere.

# A STATIC library on every platform. Keeping the native-impl OSes (Windows / Apple)
# from compiling the generic thread-pool infrastructure is the job of the per-platform
# HEADER_FILE_ONLY exclusions above (generic.cpp, futex vs generic barrier/semaphore,
# wrong-OS backends) -- NOT of dropping the library kind to INTERFACE: the
# platform-independent support TUs (hardware_concurrency.cpp, spread_chunked.cpp, the
# platform futex backend) are needed everywhere, and an INTERFACE library silently left
# them uncompiled (undefined symbols for any standalone consumer on Windows / Apple).
set( _sweater_header_only FALSE  )
set( _sweater_scope       PUBLIC )
add_library( psi_sweater STATIC ${sweater_sources} )
add_library( psi::sweater ALIAS psi_sweater )

target_include_directories( psi_sweater ${_sweater_scope} "${CMAKE_CURRENT_LIST_DIR}/include" )

# Boost (header-only here: config_ex, assert, container, core, ...). Supplied by
# the host project as the Boost::boost INTERFACE target; CPM-provided in the
# standalone build. Linked here (rather than by the consumer) so the INTERFACE
# vs PUBLIC scope stays correct for both library kinds.
if ( TARGET Boost::boost )
    target_link_libraries( psi_sweater ${_sweater_scope} Boost::boost )
endif()

# ── generic-impl backing dependencies ────────────────────────────────────────
# Only the generic implementation pulls Boost.Functionoid (the type-erased
# work_t backend) and the moodycamel concurrentqueue (the MPMC work queue);
# both are #included from headers reachable by consumers, hence PUBLIC.
if ( NOT _sweater_header_only )
    # Psi.Functionoid: prefer Psi::Functionoid from functionoid.cmake, else
    # a CPM/FetchContent population (functionoid_SOURCE_DIR) or a sibling
    # submodule (host layout: deps/psiha/functionoid) — else fetch it.
    if ( TARGET Psi::Functionoid )
        target_link_libraries( psi_sweater ${_sweater_scope} Psi::Functionoid )
    else()
    set( _sweater_functionoid_sibling "${CMAKE_CURRENT_LIST_DIR}/../functionoid/include" )
    if ( functionoid_SOURCE_DIR AND EXISTS "${functionoid_SOURCE_DIR}/include/psi/functionoid/functionoid.hpp" )
        target_include_directories( psi_sweater PUBLIC "${functionoid_SOURCE_DIR}/include" )
    elseif ( EXISTS "${_sweater_functionoid_sibling}/psi/functionoid/functionoid.hpp" )
        target_include_directories( psi_sweater PUBLIC "${_sweater_functionoid_sibling}" )
    else()
        include( FetchContent )
        FetchContent_Declare( functionoid
            GIT_REPOSITORY https://github.com/psiha/functionoid.git
            GIT_TAG        master
        )
        FetchContent_MakeAvailable( functionoid )
        target_include_directories( psi_sweater PUBLIC "${functionoid_SOURCE_DIR}/include" )
    endif()
    endif()

    # moodycamel concurrentqueue. The sweater includes it as
    # <concurrentqueue/concurrentqueue.h> (the installed-package layout), so the
    # populated dir is named `concurrentqueue` and its PARENT is put on the
    # include path (moodycamel's own target exposes the un-prefixed header).
    if ( NOT TARGET concurrentqueue AND NOT concurrentqueue_POPULATED )
        include( FetchContent )
        FetchContent_Declare( concurrentqueue
            GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
            GIT_TAG        d655418bb644b7f85159d94c591d7d983949fb81
            GIT_SHALLOW    FALSE
            SOURCE_DIR     ${CMAKE_BINARY_DIR}/_deps/cq/concurrentqueue
        )
        FetchContent_MakeAvailable( concurrentqueue )
    endif()
    target_include_directories( psi_sweater PUBLIC ${CMAKE_BINARY_DIR}/_deps/cq )
endif()
