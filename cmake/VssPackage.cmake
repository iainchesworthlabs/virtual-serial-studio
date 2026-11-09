# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installers, made with CPack. Included from the top-level CMakeLists.txt when
# VSS_INSTALL is on, after every install() rule has been declared.
#
# This file holds what every system shares. What belongs to one system is in
# cmake/packaging/VssPackage-<system>.cmake (VssPackage-darwin.cmake,
# VssPackage-linux.cmake, VssPackage-windows.cmake, the system name in lower
# case); CMake picks the file and a system without one builds no installer. Add
# a system's settings to its own file and leave this one alone.
#
# The installers hold the "runtime" install component, the same files the
# release archives hold. The "development" component (libraries and headers) is
# not shipped.

set(CPACK_PACKAGE_NAME "virtual-serial-studio")
set(CPACK_PACKAGE_VENDOR "Virtual Serial Studio contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_COMPONENTS_ALL runtime)

string(TOLOWER "${CMAKE_SYSTEM_NAME}" _vss_system_name)
set(_vss_system_packaging
  "${CMAKE_CURRENT_LIST_DIR}/packaging/VssPackage-${_vss_system_name}.cmake")
if(EXISTS "${_vss_system_packaging}")
  include("${_vss_system_packaging}")
endif()
unset(_vss_system_packaging)
unset(_vss_system_name)

include(CPack)
