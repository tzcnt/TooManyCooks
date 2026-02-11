cmake_minimum_required(VERSION 3.28)
project(TooManyCooks 
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "TooManyCooks - A runtime and concurrency library for C++20 coroutines"
)

# Require C++20 for coroutines and modules
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(TMC_BUILD_MODULE "Build the C++20 module" ON)
option(TMC_USE_HWLOC "Enable hwloc support for topology detection" OFF)
option(TMC_DEBUG_TASK_ALLOC_COUNT "Enable task allocation counting for debugging" OFF)

# Create the main module library
add_library(tmc)

# Add the module source
target_sources(tmc
    PUBLIC
        FILE_SET CXX_MODULES FILES
            modules/tmc.cppm
)

# Include directories for headers used by the module
target_include_directories(tmc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Compile definitions
# Module build requires external linkage for helper functions
target_compile_definitions(tmc PRIVATE TMC_MODULE_BUILD)

if(TMC_USE_HWLOC)
    target_compile_definitions(tmc PUBLIC TMC_USE_HWLOC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HWLOC REQUIRED hwloc)
    target_include_directories(tmc PUBLIC ${HWLOC_INCLUDE_DIRS})
    target_link_libraries(tmc PUBLIC ${HWLOC_LIBRARIES})
endif()

if(TMC_DEBUG_TASK_ALLOC_COUNT)
    target_compile_definitions(tmc PUBLIC TMC_DEBUG_TASK_ALLOC_COUNT)
endif()

# Platform-specific settings
if(UNIX AND NOT APPLE)
    target_link_libraries(tmc PUBLIC pthread)
endif()

# Installation rules
include(GNUInstallDirs)

install(TARGETS tmc
    EXPORT TooManyCooksTargets
    FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_LIBDIR}/modules
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT TooManyCooksTargets
    FILE TooManyCooksTargets.cmake
    NAMESPACE TooManyCooks::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TooManyCooks
)

# Create config file
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TooManyCooksConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/TooManyCooksConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TooManyCooks
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/TooManyCooksConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/TooManyCooksConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/TooManyCooksConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TooManyCooks
)
