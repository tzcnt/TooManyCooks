cmake_minimum_required(VERSION 3.16)
project(TooManyCooks VERSION 1.5.0 LANGUAGES CXX)

# ---- Dependency Options ----
# all options are documented in detail at https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html
option(TMC_USE_HWLOC "Enable hwloc topology-aware thread placement" ON)
option(TMC_USE_BOOST_ASIO "Use boost::asio instead of standalone asio" OFF)

# ---- Configuration Options ----
set(TMC_WORK_ITEM "CORO" CACHE STRING "Work item type: CORO or FUNCORO")
set_property(CACHE TMC_WORK_ITEM PROPERTY STRINGS "CORO" "FUNCORO")

option(TMC_TRIVIAL_TASK "Enable trivial task optimization" OFF)
option(TMC_NODISCARD_AWAIT "Enable [[nodiscard]] on co_await" OFF)

set(TMC_PRIORITY_COUNT "" CACHE STRING "Number of priority levels (1-16, empty for runtime-configurable)")

option(TMC_MORE_THREADS "Allow more than 64 threads" OFF)
option(TMC_DEBUG_TASK_ALLOC_COUNT "Track task allocation count for HALO analysis" OFF)
option(TMC_DEBUG_THREAD_CREATION "Print thread creation and work stealing info" OFF)

# ---- Build Mode Options ----
option(TMC_STANDALONE_COMPILATION "Disable header-only mode. Library will be built into the file that defines TMC_IMPL" OFF)

if(WIN32)
    option(TMC_WINDOWS_DLL "Enable Windows DLL mode" OFF)
endif()

if(TMC_WINDOWS_DLL AND NOT TMC_STANDALONE_COMPILATION)
    message(FATAL_ERROR "TMC_WINDOWS_DLL requires TMC_STANDALONE_COMPILATION=ON")
endif()

# ---- INTERFACE library target ----
add_library(TooManyCooks INTERFACE)
add_library(TooManyCooks::TooManyCooks ALIAS TooManyCooks)

target_include_directories(TooManyCooks INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(TooManyCooks INTERFACE cxx_std_20)

# Map CMake options onto INTERFACE-scoped preprocessor definitions
include(${CMAKE_CURRENT_LIST_DIR}/cmake/tmc-options.cmake)
tmc_apply_options(TooManyCooks)

# ---- Dependencies ----

# hwloc
if(TMC_USE_HWLOC)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/tmc-find-hwloc.cmake)
    tmc_find_hwloc()

    if(NOT TMC_HWLOC_FOUND)
        message(FATAL_ERROR
            "TMC_USE_HWLOC is ON but hwloc was not found. "
            "Install libhwloc-dev (Debian/Ubuntu) or set -DTMC_USE_HWLOC=OFF. "
            "On Windows, create an hwloc::hwloc imported target before add_subdirectory()."
        )
    endif()

    target_link_libraries(TooManyCooks INTERFACE hwloc::hwloc)
endif()

# pthreads
if(CMAKE_SYSTEM_NAME MATCHES "Linux|FreeBSD")
    find_package(Threads REQUIRED)
    target_link_libraries(TooManyCooks INTERFACE Threads::Threads)
endif()

# hide install target when used as a subproject
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/tmc-install.cmake)
endif()
