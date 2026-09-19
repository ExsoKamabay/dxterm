include_guard(GLOBAL)
include(CMakePackageConfigHelpers)

set(_vhdp_install_targets vhdp vhdp_static)
if(VHDP_BUILD_CLI)
  list(APPEND _vhdp_install_targets phdp)
endif()
install(TARGETS ${_vhdp_install_targets}
  EXPORT VHDPTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES
  "${PROJECT_SOURCE_DIR}/include/vhdp/vhdp.h"
  "${PROJECT_SOURCE_DIR}/include/vhdp/vhdp.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vhdp)
install(EXPORT VHDPTargets
  NAMESPACE VHDP::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VHDP)

configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/VHDPConfig.cmake.in"
  "${PROJECT_BINARY_DIR}/VHDPConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VHDP)
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/VHDPConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion)
install(FILES
  "${PROJECT_BINARY_DIR}/VHDPConfig.cmake"
  "${PROJECT_BINARY_DIR}/VHDPConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VHDP)
