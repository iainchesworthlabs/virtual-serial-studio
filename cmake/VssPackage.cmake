# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installer packages (CPack).
#
# The release archives come from `cmake --install --component runtime`. This
# module is for the installers a system's package manager or installer program
# reads: a .deb and an .rpm on Linux, and one file per other system.
#
#   cmake -DVSS_PACKAGE=ON -DVSS_PACKAGE_PRERELEASE=rc.1 build/<preset>
#   cpack --config build/<preset>/CPackConfig.cmake
#
# This file holds what every system shares. The settings of one system are in
# cmake/packaging/VssPackage-<system>.cmake, where <system> is the lower-case
# CMAKE_SYSTEM_NAME (linux, windows or darwin). A system without a file gets
# the shared settings and CPack's defaults. Add a system by adding its file; do
# not edit this one for it.

include_guard(GLOBAL)

option(VSS_PACKAGE
  "Configure CPack to build the installer packages of this system" OFF)
set(VSS_PACKAGE_PRERELEASE "" CACHE STRING
  "The pre-release label of the version being packaged, such as rc.1 for the tag v0.1.0-rc.1. Empty for a final release")

if(NOT VSS_PACKAGE)
  return()
endif()

if(NOT VSS_IS_TOP_LEVEL)
  message(FATAL_ERROR "Vss: VSS_PACKAGE is for building this project, not for using it as a subproject")
endif()

set(CPACK_PACKAGE_NAME "virtual-serial-studio")
set(CPACK_PACKAGE_VENDOR "Iain Chesworth Labs")
# The identity of the release signing key (SECURITY.md).
set(CPACK_PACKAGE_CONTACT "Iain Chesworth Labs Releases <iain.chesworth@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Playground for real, virtual and networked serial ports")
set(CPACK_PACKAGE_DESCRIPTION
  "Virtual Serial Studio creates virtual serial ports and wires them to real ports, to each other and to the network, so that serial programs can be written and tried without hardware. It has a graphical studio (vss_gui) and a command line tool (vss_cli).")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

# The pre-release label comes from the tag, which CMakeLists.txt does not know
# (project(VERSION) holds 0.1.0 for the tag v0.1.0-rc.1). Keep only the
# characters every package format accepts in a version.
string(REGEX REPLACE "[^A-Za-z0-9.+]" "." VSS_PACKAGE_PRERELEASE_SAFE "${VSS_PACKAGE_PRERELEASE}")

string(TOLOWER "${CMAKE_SYSTEM_NAME}" _vss_package_system)
set(_vss_package_file
  "${CMAKE_CURRENT_LIST_DIR}/packaging/VssPackage-${_vss_package_system}.cmake")
if(EXISTS "${_vss_package_file}")
  include("${_vss_package_file}")
else()
  message(STATUS "Vss: no package settings for ${CMAKE_SYSTEM_NAME} (${_vss_package_file}); CPack defaults apply")
endif()
unset(_vss_package_file)
unset(_vss_package_system)

include(CPack)
