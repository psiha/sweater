#############################################################################
# (c) Copyright Domagoj Saric 2021 - 2024.
#
#  Use, modification and distribution are subject to the
#  Boost Software License, Version 1.0. (See accompanying file
#  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
#  See http://www.boost.org for most recent version.
#############################################################################

set( src_root "${CMAKE_CURRENT_LIST_DIR}/include/boost/sweater" )

set( sweater_sources
    ${src_root}/spread_chunked.cpp
    ${src_root}/spread_chunked.hpp
    ${src_root}/sweater.hpp
)

set( sources_impls
    ${src_root}/impls/apple.hpp
    ${src_root}/impls/generic.cpp
    ${src_root}/impls/generic.hpp
    ${src_root}/impls/openmp.hpp
    ${src_root}/impls/single_threaded.hpp
    ${src_root}/impls/windows.hpp
)
source_group( "Impls" FILES ${sources_impls} )
list( APPEND sweater_sources ${sources_impls} )

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
    ${src_root}/threading/rw_lock.hpp
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
    ${src_root}/threading/posix/rw_lock.hpp
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
    ${src_root}/threading/windows/rw_lock.hpp
    ${src_root}/threading/windows/thread.hpp
)
source_group( "ThrdLite/Windows" FILES ${sources_threading_windows} )
list( APPEND sweater_sources ${sources_threading_windows} )
if ( NOT WIN32 )
    set_source_files_properties( ${sources_threading_windows} PROPERTIES HEADER_FILE_ONLY ON )
endif()
