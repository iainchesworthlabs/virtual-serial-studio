# SPDX-License-Identifier: GPL-3.0-or-later
#
# CPack settings for the Windows installer: one NSIS program, "Virtual Serial
# Studio". VssPackage.cmake includes this file for a Windows build and ends with
# include(CPack). The installer holds the `runtime` install component (the
# programs, the Qt libraries beside the GUI and the licence texts) and, when the
# release workflow has put it there, the virtual COM port driver and its helper
# service in bin\driver.
#
#   cpack --config build/<preset>/CPackConfig.cmake -G NSIS
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
#   - offers, on a page with the box ticked, to set up the user-mode virtual COM
#     port driver and the helper service from bin\driver (driver.nsh). /DRIVER=0
#     skips it in a silent install. If Windows does not trust the signature on the
#     driver files the driver is skipped and the installer says why; it adds no
#     certificate and does not touch test signing. The uninstaller removes them
#
# The user-mode driver is the one that ships. The kernel-mode driver needs a
# Microsoft signature to load without test signing, and nothing signs it yet; the
# user-mode driver needs a catalog signed by a publisher the machine trusts and
# no kernel signature. Studio's own "Set up Virtual Serial Studio" is built for it.
#
# There is no per-user mode. CPack's NSIS template has no choice between the two,
# the Start menu entry and the PATH would differ between them, and the driver is
# per machine. The zip is the way to run Studio without administrator rights.
#
# The release workflow signs the programs first and then packages that signed tree
# (stage, with the driver package in bin\driver) instead of installing from the
# build tree. Both variables are needed: CPACK_INSTALLED_DIRECTORIES names the
# tree, and an empty CPACK_INSTALL_CMAKE_PROJECTS stops CPack running the install
# rules on top of it, which would put unsigned copies of the programs back.
#
#   cpack ... -D "CPACK_INSTALLED_DIRECTORIES=<absolute path of stage>;/"
#             -D "CPACK_INSTALL_CMAKE_PROJECTS="

if(NOT WIN32)
  return()
endif()

set(CPACK_GENERATOR NSIS)

set(CPACK_PACKAGE_NAME "Virtual Serial Studio")
set(CPACK_PACKAGE_CONTACT "Virtual Serial Studio contributors")

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

# The licence page shows the GPL text.
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")

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

# The icon of the installer and the uninstaller, from the application icon. The
# path is one line here so that it can be repointed; without the file the
# installer has the default NSIS icon.
set(VSS_WINDOWS_ICON "${PROJECT_SOURCE_DIR}/apps/vss_gui/assets/icon/vss_gui.ico"
  CACHE FILEPATH "The .ico for the Windows installer and uninstaller")
if(EXISTS "${VSS_WINDOWS_ICON}")
  set(CPACK_NSIS_MUI_ICON "${VSS_WINDOWS_ICON}")
  set(CPACK_NSIS_MUI_UNIICON "${VSS_WINDOWS_ICON}")
endif()

# The driver and the helper service: a page to choose them, the set-up after the
# files are copied, and their removal before the uninstaller deletes the files.
# CPack copies these values into CPackConfig.cmake without escaping them, so they
# use NSIS's single quotes and doubled backslashes. !include wants a Windows path.
file(TO_NATIVE_PATH "${CMAKE_CURRENT_LIST_DIR}/windows/driver.nsh" _vss_driver_nsh)
string(REPLACE "\\" "\\\\" _vss_driver_nsh "${_vss_driver_nsh}")
set(CPACK_NSIS_DEFINES
  "!define VSS_REGISTRY_KEY '${CPACK_PACKAGE_INSTALL_REGISTRY_KEY}'
!include '${_vss_driver_nsh}'")
unset(_vss_driver_nsh)
set(CPACK_NSIS_PAGE_COMPONENTS "Page custom VssDriverPageCreate VssDriverPageLeave")
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "  Call VssInstallDriver")
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
  "  Call un.VssRemoveDriver
  Delete '$INSTDIR\\\\driver-setup.log'")
