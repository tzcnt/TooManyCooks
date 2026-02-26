cmake_minimum_required(VERSION 3.16)
project(TooManyCooks LANGUAGES CXX)

# ---- Dependency Options ----
option(TMC_USE_HWLOC "Enable hwloc topology-aware thread placement" ON)
option(TMC_USE_BOOST_ASIO "Use boost::asio instead of standalone asio" OFF)

# ---- Configuration Options ----
set(TMC_WORK_ITEM "CORO" CACHE STRING "Work item type: CORO or FUNCORO")
set_property(CACHE TMC_WORK_ITEM PROPERTY STRINGS "CORO" "FUNCORO")

option(TMC_TRIVIAL_TASK "Enable trivial task optimization" OFF)
option(TMC_NODISCARD_AWAIT "Enable [[nodiscard]] on co_await" OFF)

set(TMC_PRIORITY_COUNT "" CACHE STRING "Number of priority levels (1-16, empty for default)")

option(TMC_MORE_THREADS "Allow more than 64 threads" OFF)
option(TMC_DEBUG_TASK_ALLOC_COUNT "Track task allocation count for HALO analysis" OFF)
option(TMC_DEBUG_THREAD_CREATION "Print thread creation and work stealing info" OFF)

# ---- Build Mode Options ----
option(TMC_STANDALONE_COMPILATION "Enable standalone compilation mode" OFF)
if(WIN32)
    option(TMC_WINDOWS_DLL "Enable Windows DLL mode" OFF)
endif()

# ---- INTERFACE library target ----
add_library(tmc INTERFACE)
add_library(tmc::tmc ALIAS tmc)

target_include_directories(tmc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(tmc INTERFACE cxx_std_20)

# ---- Propagate preprocessor defines ----
if(TMC_USE_HWLOC)
    target_compile_definitions(tmc INTERFACE TMC_USE_HWLOC)
endif()

if(TMC_USE_BOOST_ASIO)
    target_compile_definitions(tmc INTERFACE TMC_USE_BOOST_ASIO)
endif()

if(TMC_WORK_ITEM STREQUAL "FUNCORO")
    target_compile_definitions(tmc INTERFACE "TMC_WORK_ITEM=FUNCORO")
endif()

if(TMC_TRIVIAL_TASK)
    target_compile_definitions(tmc INTERFACE TMC_TRIVIAL_TASK)
endif()

if(TMC_NODISCARD_AWAIT)
    target_compile_definitions(tmc INTERFACE TMC_NODISCARD_AWAIT)
endif()

if(NOT "${TMC_PRIORITY_COUNT}" STREQUAL "")
    target_compile_definitions(tmc INTERFACE "TMC_PRIORITY_COUNT=${TMC_PRIORITY_COUNT}")
endif()

if(TMC_MORE_THREADS)
    target_compile_definitions(tmc INTERFACE TMC_MORE_THREADS)
endif()

if(TMC_DEBUG_TASK_ALLOC_COUNT)
    target_compile_definitions(tmc INTERFACE TMC_DEBUG_TASK_ALLOC_COUNT)
endif()

if(TMC_DEBUG_THREAD_CREATION)
    target_compile_definitions(tmc INTERFACE TMC_DEBUG_THREAD_CREATION)
endif()

if(TMC_STANDALONE_COMPILATION)
    target_compile_definitions(tmc INTERFACE TMC_STANDALONE_COMPILATION)
endif()

if(WIN32 AND TMC_WINDOWS_DLL)
    target_compile_definitions(tmc INTERFACE TMC_WINDOWS_DLL)
endif()

# ---- Dependencies ----

# hwloc
if(TMC_USE_HWLOC)
    if(NOT TARGET hwloc::hwloc)
        find_library(HWLOC_LIBRARY NAMES hwloc)
        find_path(HWLOC_INCLUDE_DIR NAMES hwloc.h)
        if(HWLOC_LIBRARY AND HWLOC_INCLUDE_DIR)
            message(STATUS "TooManyCooks: Found hwloc: ${HWLOC_LIBRARY}")
            add_library(hwloc::hwloc UNKNOWN IMPORTED)
            set_target_properties(hwloc::hwloc PROPERTIES
                IMPORTED_LOCATION "${HWLOC_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${HWLOC_INCLUDE_DIR}"
            )
        else()
            message(FATAL_ERROR
                "TMC_USE_HWLOC is ON but hwloc was not found. "
                "Install libhwloc-dev (Debian/Ubuntu) or set -DTMC_USE_HWLOC=OFF. "
                "On Windows, create an hwloc::hwloc imported target before add_subdirectory()."
            )
        endif()
    endif()
    target_link_libraries(tmc INTERFACE hwloc::hwloc)
endif()

# pthreads
if(CMAKE_SYSTEM_NAME MATCHES "Linux|FreeBSD")
    find_package(Threads REQUIRED)
    target_link_libraries(tmc INTERFACE Threads::Threads)
endif()
