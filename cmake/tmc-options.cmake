# See documentation at https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html
function(tmc_apply_options target)
    if(TMC_USE_HWLOC)
        target_compile_definitions(${target} INTERFACE TMC_USE_HWLOC)
    endif()

    if(TMC_USE_BOOST_ASIO)
        target_compile_definitions(${target} INTERFACE TMC_USE_BOOST_ASIO)
    endif()

    if(TMC_WORK_ITEM STREQUAL "FUNCORO")
        target_compile_definitions(${target} INTERFACE "TMC_WORK_ITEM=FUNCORO")
    endif()

    if(TMC_TRIVIAL_TASK)
        target_compile_definitions(${target} INTERFACE TMC_TRIVIAL_TASK)
    endif()

    if(TMC_NODISCARD_AWAIT)
        target_compile_definitions(${target} INTERFACE TMC_NODISCARD_AWAIT)
    endif()

    if(NOT "${TMC_PRIORITY_COUNT}" STREQUAL "")
        target_compile_definitions(${target} INTERFACE "TMC_PRIORITY_COUNT=${TMC_PRIORITY_COUNT}")
    endif()

    if(TMC_MORE_THREADS)
        target_compile_definitions(${target} INTERFACE TMC_MORE_THREADS)
    endif()

    if(TMC_DEBUG_TASK_ALLOC_COUNT)
        target_compile_definitions(${target} INTERFACE TMC_DEBUG_TASK_ALLOC_COUNT)
    endif()

    if(TMC_DEBUG_THREAD_CREATION)
        target_compile_definitions(${target} INTERFACE TMC_DEBUG_THREAD_CREATION)
    endif()

    if(TMC_STANDALONE_COMPILATION AND NOT TMC_WINDOWS_DLL)
        # TMC_WINDOWS_DLL requires TMC_STANDALONE_COMPILATION at the CMake level for clarity,
        # but as defines, they are redundant - TMC_WINDOWS_DLL implies TMC_STANDALONE_COMPILATION
        target_compile_definitions(${target} INTERFACE TMC_STANDALONE_COMPILATION)
    endif()

    if(TMC_WINDOWS_DLL)
        target_compile_definitions(${target} INTERFACE TMC_WINDOWS_DLL)
    endif()
endfunction()
