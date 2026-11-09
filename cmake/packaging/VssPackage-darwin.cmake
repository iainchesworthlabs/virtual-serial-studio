# SPDX-License-Identifier: GPL-3.0-or-later
#
# macOS installer: a disk image (.dmg) made by CPack's DragNDrop generator.
# Included by cmake/VssPackage.cmake.
#
# The volume holds vss_gui.app, a link to /Applications to drag it onto, and the
# rest of the runtime component: bin/vss_cli and the licence texts. The command
# line tool is not inside the application bundle on purpose. The bundle is
# sealed by macdeployqt (and later by tools/macos/sign-and-notarize.sh), and a
# file added to it afterwards breaks the seal. The CLI therefore sits beside the
# app and the user copies it to a directory on PATH (docs/releasing.md).
#
# The image is arm64: that is the only macOS package the release builds.
# Signing is not done here. When VSS_SIGN_IDENTITY and VSS_NOTARY_PROFILE are in
# the environment of the cpack run, cmake/packaging/VssPackage-darwin-sign.cmake signs and
# notarises the app before the image is made; without them the image is
# unsigned.

set(CPACK_GENERATOR "DragNDrop")
set(CPACK_PACKAGE_FILE_NAME
  "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-macos-${CMAKE_SYSTEM_PROCESSOR}")

# One image holding every selected component, not one per component.
set(CPACK_COMPONENTS_GROUPING "ALL_COMPONENTS_IN_ONE")

set(CPACK_DMG_VOLUME_NAME "Virtual Serial Studio")
set(CPACK_DMG_FORMAT "UDZO")

# DragNDrop adds the Applications link itself; CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK
# would remove it.

set(CPACK_VSS_SIGN_SCRIPT
  "${PROJECT_SOURCE_DIR}/tools/macos/sign-and-notarize.sh")
set(CPACK_PRE_BUILD_SCRIPTS
  "${PROJECT_SOURCE_DIR}/cmake/packaging/VssPackage-darwin-sign.cmake")
