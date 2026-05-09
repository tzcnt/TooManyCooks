include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(TooManyCooks_INSTALL_CMAKEDIR
  "${CMAKE_INSTALL_LIBDIR}/cmake/TooManyCooks"
  CACHE STRING "CMake package config location relative to the install prefix"
)

mark_as_advanced(TooManyCooks_INSTALL_CMAKEDIR)

install(TARGETS TooManyCooks
  EXPORT TooManyCooksTargets
  COMPONENT TooManyCooks_Development
)

install(DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT TooManyCooks_Development
)

install(FILES LICENSE
  DESTINATION licenses
  COMPONENT TooManyCooks_Development
)

install(FILES cmake/tmc-find-hwloc.cmake
  DESTINATION ${TooManyCooks_INSTALL_CMAKEDIR}
  COMPONENT TooManyCooks_Development
)

install(EXPORT TooManyCooksTargets
  DESTINATION ${TooManyCooks_INSTALL_CMAKEDIR}
  NAMESPACE TooManyCooks::
  FILE TooManyCooksTargets.cmake
  COMPONENT TooManyCooks_Development
)

configure_package_config_file(
  ${CMAKE_CURRENT_LIST_DIR}/TooManyCooksConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/TooManyCooksConfig.cmake
  INSTALL_DESTINATION ${TooManyCooks_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/TooManyCooksConfigVersion.cmake
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/TooManyCooksConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/TooManyCooksConfigVersion.cmake
  DESTINATION ${TooManyCooks_INSTALL_CMAKEDIR}
  COMPONENT TooManyCooks_Development
)

# eventually, if we want to build installer packages
# include(CPack)
