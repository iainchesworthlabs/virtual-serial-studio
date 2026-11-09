# SPDX-License-Identifier: GPL-3.0-or-later
#
# CPack settings for the Windows installer: one NSIS program, "Virtual Serial
# Studio", built from the `runtime` install component (the programs, the Qt
# libraries beside the GUI and the licence texts). CMakeLists.txt includes this
# file for a Windows build and no other; the settings of the other systems are in
# their own VssPackaging<System>.cmake files.
#
#   cpack --config build/<preset>/CPackConfig.cmake -G NSIS -C Release
#
# makensis must be on PATH or in its usual place (CMake looks in the registry
# and in Program Files). The release workflow installs the version pinned in
# .github/toolchain-versions.json.
#
# What the installer does:
#
#   - installs for all users, into %ProgramFiles%\Virtual Serial Studio, and so
#     asks for administrator rights once, when the installer starts
#   - puts "Virtual Serial Studio" (vss_gui.exe) in the Start menu, with an
#     uninstall shortcut
#   - registers an uninstaller and an Apps & features entry
#   - removes an earlier version first, so an upgrade replaces the files and
#     leaves nothing behind
#   - offers to add the folder of vss_cli.exe to the PATH. The page defaults to
#     "Do not add", and a silent install (/S) adds nothing
#
# It does not install the virtual COM port driver or the helper service. Those
# are not signed for production yet; docs/releasing.md says what is left.
#
# There is no per-user mode. CPack's NSIS template has no choice between the two,
# the Start menu entry and the PATH would differ between them, and the driver set
# up that follows is per machine anyway. The zip is the way to run Studio without
# administrator rights.
#
# By default CPack runs the install rules of the `runtime` component. The release
# workflow signs the programs first and then packages that signed tree instead. Both
# variables are needed: CPACK_INSTALLED_DIRECTORIES names the tree, and an empty
# CPACK_INSTALL_CMAKE_PROJECTS stops CPack running the install rules on top of it,
# which would put unsigned copies of the programs back.
#
#   cpack ... -D "CPACK_INSTALLED_DIRECTORIES=<absolute path of stage>;/"
#             -D "CPACK_INSTALL_CMAKE_PROJECTS="

include_guard(GLOBAL)

if(NOT WIN32)
  return()
endif()

set(CPACK_GENERATOR NSIS)

set(CPACK_PACKAGE_NAME "Virtual Serial Studio")
set(CPACK_PACKAGE_VENDOR "Virtual Serial Studio contributors")
set(CPACK_PACKAGE_CONTACT "Virtual Serial Studio contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
  set(_vss_package_arch arm64)
else()
  set(_vss_package_arch x86_64)
endif()
set(CPACK_PACKAGE_FILE_NAME
  "virtual-serial-studio-${PROJECT_VERSION}-windows-${_vss_package_arch}")
unset(_vss_package_arch)

# No version in the folder or in the registry key: an upgrade has to find the
# earlier version's uninstaller and install into the same place.
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Virtual Serial Studio")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "VirtualSerialStudio")

# Only what a release holds. The development component (libraries and headers)
# is not installed.
set(CPACK_INSTALL_CMAKE_PROJECTS
  "${CMAKE_BINARY_DIR};${PROJECT_NAME};runtime;/")

# The licence page shows the GPL text.
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_LIST_DIR}/../LICENSE")

# The Start menu entry. CPack looks for bin/vss_gui.exe.
set(CPACK_PACKAGE_EXECUTABLES "vss_gui" "Virtual Serial Studio")

set(CPACK_NSIS_PACKAGE_NAME "Virtual Serial Studio")
set(CPACK_NSIS_DISPLAY_NAME "Virtual Serial Studio")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_MODIFY_PATH ON)
set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\vss_gui.exe")
set(CPACK_NSIS_URL_INFO_ABOUT "${PROJECT_HOMEPAGE_URL}")
set(CPACK_NSIS_HELP_LINK "${PROJECT_HOMEPAGE_URL}")
set(CPACK_NSIS_CONTACT "${CPACK_PACKAGE_CONTACT}")

include(CPack)
