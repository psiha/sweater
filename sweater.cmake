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
    ${src_root}/impls/libuv.cpp
    ${src_root}/impls/libuv.hpp
    ${src_root}/impls/openmp.hpp
    ${src_root}/impls/single_threaded.hpp
    ${src_root}/impls/windows.hpp
)
source_group( "Impls" FILES ${sources_impls} )
list( APPEND sweater_sources ${sources_impls} )
# A host project may override the implementation the platform would select on
# its own (PSI_SWEATER_IMPL=generic to run the own thread pool on Windows /
# Apple, say, to compare it against the native one). Only then does the generic
# pool's .cpp have to be compiled on those platforms.
set( PSI_SWEATER_IMPL "" CACHE STRING "Override the psi::sweater implementation (generic/windows/apple/libuv/openmp/single_threaded); empty = let the platform choose" )
if ( ( WIN32 OR APPLE ) AND NOT ( PSI_SWEATER_IMPL STREQUAL "generic" ) )
    # Windows and Apple use native impls (windows.hpp / apple.hpp); the generic
    # thread pool's .cpp is excluded from compilation on those platforms.
    set_source_files_properties( ${src_root}/impls/generic.cpp PROPERTIES HEADER_FILE_ONLY ON )
endif()
# The libuv backend's TU compiles only when libuv headers are reachable. A
# host project that supplies its own libuv headers (e.g. a Node.js embedder
# using the Node SDK's bundled uv.h) can point PSI_SWEATER_LIBUV_INCLUDE_DIR
# at them instead of relying on a system install.
set( PSI_SWEATER_LIBUV_INCLUDE_DIR "" CACHE PATH "Directory containing uv.h for the libuv psi::sweater backend; empty = search system paths" )
if ( NOT PSI_SWEATER_LIBUV_INCLUDE_DIR )
    find_path( PSI_SWEATER_LIBUV_INCLUDE_DIR_FOUND NAMES uv.h )
    if ( PSI_SWEATER_LIBUV_INCLUDE_DIR_FOUND )
        set( PSI_SWEATER_LIBUV_INCLUDE_DIR "${PSI_SWEATER_LIBUV_INCLUDE_DIR_FOUND}" )
    endif()
endif()
if ( NOT PSI_SWEATER_LIBUV_INCLUDE_DIR )
    set_source_files_properties( ${src_root}/impls/libuv.cpp PROPERTIES HEADER_FILE_ONLY ON )
endif()

set( sources_queues
    ${src_root}/queues/mpmc_moodycamel.hpp
)
source_group( "Queues" FILES ${sources_queues} )
list( APPEND sweater_sources ${sources_queues} )

# See the Outcome linkage block (below the Boost::boost linkage) for what
# these gate; declared here, ahead of the sources_threading list below, so
# outcome_future.hpp's presence in that list depends on the same read of
# PSI_SWEATER_WITH_OUTCOME the linkage block uses.
option( PSI_SWEATER_WITH_OUTCOME "Build the Outcome-based dispatch_outcome()/outcome_future<T> variant" OFF )
option( PSI_SWEATER_OUTCOME_STANDALONE "When PSI_SWEATER_WITH_OUTCOME is ON: fetch standalone (non-Boost) Outcome via CPM instead of using Boost::boost's boost/outcome.hpp" ON )

set( sources_threading
    ${src_root}/threading/barrier.hpp
    ${src_root}/threading/futex.hpp
    ${src_root}/threading/futex_barrier.cpp
    ${src_root}/threading/futex_barrier.hpp
    ${src_root}/threading/futex_rw_mutex.cpp
    ${src_root}/threading/futex_rw_mutex.hpp
    ${src_root}/threading/futex_semaphore.cpp
    ${src_root}/threading/generic_barrier.cpp
    ${src_root}/threading/generic_barrier.hpp
    ${src_root}/threading/generic_semaphore.cpp
    ${src_root}/threading/hardware_concurrency.cpp
    ${src_root}/threading/hardware_concurrency.hpp
    ${src_root}/threading/condvar.hpp
    ${src_root}/threading/future.hpp
    ${src_root}/threading/mutex.hpp
    ${src_root}/threading/rw_mutex.hpp
    ${src_root}/threading/semaphore.hpp
    ${src_root}/threading/thread.hpp
)
if ( PSI_SWEATER_WITH_OUTCOME )
    list( APPEND sources_threading ${src_root}/threading/outcome_future.hpp )
endif()
source_group( "ThrdLite" FILES ${sources_threading} )
list( APPEND sweater_sources ${sources_threading} )

# Apple's embedded OSes (iOS/tvOS/watchOS/visionOS) have no futex backend at
# all (the __ulock syscalls are private -- App Store rejection material; see
# futex.hpp's PSI_THRD_LITE_HAS_FUTEX) so every futex-backed TU is excluded
# there. CMAKE_SYSTEM_NAME is "Darwin" only for macOS proper.
if ( APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" )
    set( sweater_apple_embedded TRUE  )
else()
    set( sweater_apple_embedded FALSE )
endif()

# Both the BARRIER and the SEMAPHORE are futex-backed on every platform with
# a futex backend (macOS through __ulock); Apple's embedded OSes have no
# futex backend at all and keep the condvar-based generic variants (see
# barrier.hpp / semaphore.hpp -- the latter also documents the semaphore
# protocol history behind this selection).
# PSI_SWEATER_FORCE_CONDVAR_SEMAPHORE forces the condvar semaphore anywhere,
# purely as an A/B knob.
option( PSI_SWEATER_FORCE_CONDVAR_SEMAPHORE "Force the condvar-based semaphore (A/B knob)" OFF )
if ( sweater_apple_embedded )
    set_source_files_properties( ${src_root}/threading/futex_barrier.cpp   PROPERTIES HEADER_FILE_ONLY ON )
else()
    set_source_files_properties( ${src_root}/threading/generic_barrier.cpp PROPERTIES HEADER_FILE_ONLY ON )
endif()
if ( sweater_apple_embedded OR PSI_SWEATER_FORCE_CONDVAR_SEMAPHORE )
    set_source_files_properties( ${src_root}/threading/futex_semaphore.cpp   PROPERTIES HEADER_FILE_ONLY ON )
else()
    set_source_files_properties( ${src_root}/threading/generic_semaphore.cpp PROPERTIES HEADER_FILE_ONLY ON )
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


# __ulock_wait/__ulock_wake -- PRIVATE Darwin syscalls, see the design-doc
# comment at the top of apple/futex.cpp (the single place to swap to the
# public os_sync_wait_on_address API when the deployment floor allows). Now a
# load-bearing backend on macOS: the futex-based BARRIER, SEMAPHORE (see
# semaphore.hpp's selection-history comment) and futex_rw_mutex
# run on it. macOS ONLY: private syscalls
# are App Store rejection material on Apple's embedded OSes, which therefore
# have no futex backend at all (see futex.hpp).
set( sources_threading_apple
    ${src_root}/threading/apple/futex.cpp
)
source_group( "ThrdLite/Apple" FILES ${sources_threading_apple} )
list( APPEND sweater_sources ${sources_threading_apple} )
if ( NOT APPLE OR sweater_apple_embedded )
    set_source_files_properties( ${sources_threading_apple} PROPERTIES HEADER_FILE_ONLY ON )
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
if ( PSI_SWEATER_LIBUV_INCLUDE_DIR )
    target_include_directories( psi_sweater PRIVATE "${PSI_SWEATER_LIBUV_INCLUDE_DIR}" )
endif()

# PUBLIC: every TU that includes sweater.hpp must select the same implementation
# or the ODR checks (and the impl's own types) diverge between the library and
# its consumers.
if ( PSI_SWEATER_IMPL )
    target_compile_definitions( psi_sweater ${_sweater_scope} PSI_SWEATER_IMPL=${PSI_SWEATER_IMPL} )
endif()
# PUBLIC for the same ODR reason: semaphore.hpp's layout selection reaches
# consumers through impls/generic.hpp.
if ( PSI_SWEATER_FORCE_CONDVAR_SEMAPHORE )
    target_compile_definitions( psi_sweater ${_sweater_scope} PSI_SWEATER_FORCE_CONDVAR_SEMAPHORE )
endif()

if ( WIN32 )
    # windows/futex.cpp calls WaitOnAddress/WakeByAddressSingle/WakeByAddressAll,
    # resolved by Synchronization.lib -- not linked by default and previously never
    # missed because nothing on Windows referenced psi::thrd_lite::futex until
    # futex_rw_mutex.hpp (windows/condvar.hpp, mutex.hpp, rw_mutex.hpp are all
    # SRWLOCK/CONDITION_VARIABLE-based and never touch this backend).
    target_link_libraries( psi_sweater ${_sweater_scope} Synchronization )
endif()

# Boost (header-only here: config_ex, assert, container, core, ...). Supplied by
# the host project as the Boost::boost INTERFACE target; CPM-provided in the
# standalone build. Linked here (rather than by the consumer) so the INTERFACE
# vs PUBLIC scope stays correct for both library kinds.
if ( TARGET Boost::boost )
    target_link_libraries( psi_sweater ${_sweater_scope} Boost::boost )
endif()

# ── Outcome (optional, opt-in) ───────────────────────────────────────────────
# psi::thrd_lite::outcome_promise/outcome_future (threading/outcome_future.hpp)
# and shop::dispatch_outcome() build atop ned14/outcome's outcome<T> (value /
# std::error_code / std::exception_ptr) as a THIRD alternative to dispatch()/
# dispatch_lite() -- always noexcept to CONSUME (no throwing get(); callers
# check has_value()/has_exception()). Off by default: most consumers of this
# library don't want a new dependency pulled in just for this. Options
# declared near sources_threading above (outcome_future.hpp's inclusion in
# that list depends on PSI_SWEATER_WITH_OUTCOME too); resolved here.
# ON  (default) PSI_SWEATER_OUTCOME_STANDALONE: fetch the standalone
#     (non-Boost) ned14/outcome single header via CPM. What sweater's own
#     gh-actions CI uses -- the CPM-assembled Boost::boost aggregate above
#     does not include Outcome.
# OFF: use <boost/outcome.hpp> from an already-existing Boost::boost target
#     instead (a host project supplying a full, real Boost distribution --
#     IDE/vcpkg/system install -- already has it there; no extra fetch, no
#     second copy of Outcome alongside the host's own).
if ( PSI_SWEATER_WITH_OUTCOME )
    target_compile_definitions( psi_sweater ${_sweater_scope} PSI_SWEATER_HAS_OUTCOME=1 )
    if ( PSI_SWEATER_OUTCOME_STANDALONE )
        target_compile_definitions( psi_sweater ${_sweater_scope} PSI_SWEATER_OUTCOME_STANDALONE=1 )
        # A single amalgamated header is all that's needed (no status-code/
        # wg14_result/doc submodules, no CMake support in that repo) -- fetch
        # it directly, same file(DOWNLOAD) pattern already used above for
        # get_cpm.cmake itself, rather than CPMAddPackage cloning the whole
        # repo (and its submodules) just to reach one file.
        set( _sweater_outcome_dir "${CMAKE_CURRENT_BINARY_DIR}/deps/outcome-standalone" )
        file( DOWNLOAD
            "https://raw.githubusercontent.com/ned14/outcome/v2.2.15/single-header/outcome.hpp"
            "${_sweater_outcome_dir}/outcome.hpp"
            EXPECTED_HASH SHA256=c83881b9d0866e9b39b87888238ae6169347e1b442a800d2a20c1fefff362907
        )
        add_library( sweater_outcome_standalone INTERFACE )
        target_include_directories( sweater_outcome_standalone INTERFACE "${_sweater_outcome_dir}" )
        target_link_libraries( psi_sweater ${_sweater_scope} sweater_outcome_standalone )
    else()
        target_compile_definitions( psi_sweater ${_sweater_scope} PSI_SWEATER_OUTCOME_STANDALONE=0 )
        if ( NOT TARGET Boost::boost )
            message( FATAL_ERROR "PSI_SWEATER_WITH_OUTCOME=ON with PSI_SWEATER_OUTCOME_STANDALONE=OFF requires an existing Boost::boost target providing boost/outcome.hpp (a full Boost distribution, not the config_ex/assert/container/core/preprocessor aggregate this standalone build assembles)" )
        endif()
        # boost/outcome.hpp is already reachable through the Boost::boost link above.
    endif()
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
