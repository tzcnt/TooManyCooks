function(tmc_find_hwloc)
    if(NOT TARGET hwloc::hwloc)
        find_package(hwloc CONFIG QUIET)
    endif()

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
        endif()
    endif()

    if(TARGET hwloc::hwloc)
        set(TMC_HWLOC_FOUND TRUE PARENT_SCOPE)
    else()
        set(TMC_HWLOC_FOUND FALSE PARENT_SCOPE)
    endif()
endfunction()
