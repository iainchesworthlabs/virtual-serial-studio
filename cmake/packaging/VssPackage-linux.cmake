# SPDX-License-Identifier: GPL-3.0-or-later
#
# Linux packages: a .deb for Debian and Ubuntu and an .rpm for Fedora and Red
# Hat. Included by cmake/VssPackage.cmake, which holds the settings every system
# shares. Nothing happens here unless VSS_PACKAGE is ON, because the install
# rules below write to absolute paths that a plain `cmake --install` of every
# component must not touch.
#
#   cmake -DVSS_PACKAGE=ON -DVSS_PACKAGE_PRERELEASE=rc.1 build/<preset>
#   cpack --config build/<preset>/CPackConfig.cmake -G "DEB;RPM"
#
# What goes in
#   The "runtime" install component, unchanged: bin/vss_gui, bin/vss_cli, the
#   Qt libraries in lib/ and the Qt plug-ins in plugins/, which the release
#   archive holds too. The "integration" component below adds what only a
#   system package can: the links in /usr/bin, the launcher entry, the icon
#   and the licence texts. The libraries and headers (the "development"
#   component) are not packaged.
#
# Where it goes: /opt/virtual-serial-studio, linked from /usr/bin
#   The package carries its own Qt (below) and the programs find it through the
#   run path $ORIGIN/../lib that vss_gui is linked with, which is the layout the
#   release archive has and the release job tests. Installing that tree under
#   /usr would put libQt6*.so* in /usr/lib, where they clash with the
#   distribution's own Qt packages and shadow them for other programs. A private
#   tree under /opt, as the Filesystem Hierarchy Standard provides for add-on
#   packages, avoids both, and the links in /usr/bin put the programs on PATH
#   (the loader resolves $ORIGIN from the real file, so they work through the
#   links).
#
# Qt is bundled, not a dependency
#   vss_gui is built and tested against the Qt kit pinned in
#   .github/toolchain-versions.json (6.12.0), which is newer than the Qt the
#   distributions this package targets ship. The programs would need a
#   matching Qt from the distribution, and the package would run something
#   other than what the release job tested. The cost is the size of the package
#   and that Qt's fixes arrive with this package, not with the distribution.
#   The package depends on the system libraries that Qt and its xcb platform
#   plug-in use, which it does not carry.

option(VSS_PACKAGE
  "Configure CPack to build the deb and rpm of the Linux release" OFF)
set(VSS_PACKAGE_PRERELEASE "" CACHE STRING
  "The pre-release label of the version being packaged, such as rc.1 for the tag v0.1.0-rc.1. Empty for a final release")
if(NOT VSS_PACKAGE)
  return()
endif()

# The identity of the release signing key (SECURITY.md).
set(CPACK_PACKAGE_CONTACT "Iain Chesworth Labs Releases <iain.chesworth@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION
  "Virtual Serial Studio creates virtual serial ports and wires them to real ports, to each other and to the network, so that serial programs can be written and tried without hardware. It has a graphical studio (vss_gui) and a command line tool (vss_cli).")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

# The pre-release label comes from the tag, which CMakeLists.txt does not know
# (project(VERSION) holds 0.1.0 for the tag v0.1.0-rc.1). Keep only the
# characters every package format accepts in a version.
string(REGEX REPLACE "[^A-Za-z0-9.+]" "." VSS_PACKAGE_PRERELEASE_SAFE "${VSS_PACKAGE_PRERELEASE}")

# The architecture the package is labelled with comes from the toolchain.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(_vss_deb_arch "arm64")
  set(_vss_rpm_arch "aarch64")
else()
  set(_vss_deb_arch "amd64")
  set(_vss_rpm_arch "x86_64")
endif()

set(VSS_PACKAGE_PREFIX "/opt/virtual-serial-studio")
set(VSS_PACKAGE_DOCDIR "/usr/share/doc/virtual-serial-studio")
set(VSS_PACKAGE_LINUX_DIR "${CMAKE_SOURCE_DIR}/packaging/linux")
# The launcher icon. One line to repoint it at the application icon in
# apps/vss_gui/assets/icon when that is on develop.
set(VSS_PACKAGE_LINUX_ICON "${VSS_PACKAGE_LINUX_DIR}/virtual-serial-studio.svg")

# ---------------------------------------------------------------------------
# What the package holds
# ---------------------------------------------------------------------------

# The integration component installs to absolute paths, so CPack stages the
# files with DESTDIR. Without it CPack would install to the real /usr.
install(FILES "${VSS_PACKAGE_LINUX_DIR}/virtual-serial-studio.desktop"
  DESTINATION /usr/share/applications COMPONENT integration)
install(FILES "${VSS_PACKAGE_LINUX_ICON}" RENAME virtual-serial-studio.svg
  DESTINATION /usr/share/icons/hicolor/scalable/apps COMPONENT integration)
install(FILES
    "${VSS_PACKAGE_LINUX_DIR}/copyright"
    "${CMAKE_SOURCE_DIR}/LICENSE"
    "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
  DESTINATION "${VSS_PACKAGE_DOCDIR}" COMPONENT integration)
install(FILES
    "${CMAKE_SOURCE_DIR}/LICENSES/BSL-1.0.txt"
    "${CMAKE_SOURCE_DIR}/LICENSES/GPL-3.0-or-later.txt"
  DESTINATION "${VSS_PACKAGE_DOCDIR}/LICENSES" COMPONENT integration)
install(CODE "
  file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
  foreach(_program vss_gui vss_cli)
    file(REMOVE \"\$ENV{DESTDIR}/usr/bin/\${_program}\")
    file(CREATE_LINK \"${VSS_PACKAGE_PREFIX}/bin/\${_program}\"
      \"\$ENV{DESTDIR}/usr/bin/\${_program}\" SYMBOLIC)
  endforeach()
" COMPONENT integration)

set(CPACK_GENERATOR "DEB;RPM")
# With DESTDIR, CPack installs to CMAKE_INSTALL_PREFIX and ignores
# CPACK_PACKAGING_INSTALL_PREFIX, so the prefix is set here. A build with
# VSS_PACKAGE=ON installs to /opt/virtual-serial-studio by default; it is the
# build that makes the packages.
set(CMAKE_INSTALL_PREFIX "${VSS_PACKAGE_PREFIX}" CACHE PATH
  "Install prefix of the Linux packages" FORCE)
set(CPACK_PACKAGING_INSTALL_PREFIX "${VSS_PACKAGE_PREFIX}")
set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGE_RELOCATABLE OFF)
set(CPACK_STRIP_FILES ON)
# One package of the two components, not one package for each.
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL runtime integration)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)

# ---------------------------------------------------------------------------
# Version
#   0.1.0 final        deb 0.1.0-1             rpm 0.1.0-1
#   0.1.0-rc.1         deb 0.1.0~rc.1-1        rpm 0.1.0-0.rc.1
# A tilde sorts before everything, so 0.1.0~rc.1 is older than 0.1.0. RPM has no
# tilde in a Release, so the pre-release is a Release that starts with 0, which
# is older than the 1 of the final release.
# ---------------------------------------------------------------------------

set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
set(CPACK_RPM_PACKAGE_RELEASE "1")
set(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}")
if(VSS_PACKAGE_PRERELEASE_SAFE)
  set(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}~${VSS_PACKAGE_PRERELEASE_SAFE}")
  set(CPACK_RPM_PACKAGE_RELEASE "0.${VSS_PACKAGE_PRERELEASE_SAFE}")
endif()
set(CPACK_RPM_PACKAGE_VERSION "${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Debian
# ---------------------------------------------------------------------------

set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_vss_deb_arch}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "electronics")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${PROJECT_HOMEPAGE_URL}")

# dpkg-shlibdeps cannot follow the private run path, so the dependencies are
# written out: the system libraries that vss_gui, vss_cli, the Qt libraries and
# the xcb platform plug-in are linked to (readelf -d lists them) and that the
# package does not carry. tools/ci/check_linux_packages.sh runs ldd over the
# installed tree in a clean container, so a library missing here fails the
# release job.
#   glibc      2.42: the floor of the check-glibc-floor step in release.yml
#   libstdc++  16:   the programs are built with GCC 16 and use its library
set(VSS_DEB_DEPENDS
  "libc6 (>= 2.42)"
  "libstdc++6 (>= 16)"
  "libgcc-s1 (>= 16)"
  "libbrotli1"
  "libdbus-1-3"
  "libegl1"
  "libfontconfig1"
  "libfreetype6"
  "libgl1"
  "libglib2.0-0t64 | libglib2.0-0"
  "libglx0"
  "libgssapi-krb5-2"
  "libopengl0"
  "libx11-6"
  "libx11-xcb1"
  "libxcb-cursor0"
  "libxcb-glx0"
  "libxcb-icccm4"
  "libxcb-image0"
  "libxcb-keysyms1"
  "libxcb-randr0"
  "libxcb-render-util0"
  "libxcb-render0"
  "libxcb-shape0"
  "libxcb-shm0"
  "libxcb-sync1"
  "libxcb-util1"
  "libxcb-xfixes0"
  "libxcb-xkb1"
  "libxcb1"
  "libxkbcommon-x11-0"
  "libxkbcommon0"
  "zlib1g"
  "libzstd1")
list(JOIN VSS_DEB_DEPENDS ", " CPACK_DEBIAN_PACKAGE_DEPENDS)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
# Studio offers to add the user to the serial port group through pkexec.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pkexec")

# ---------------------------------------------------------------------------
# Red Hat
# ---------------------------------------------------------------------------

set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
set(CPACK_RPM_PACKAGE_ARCHITECTURE "${_vss_rpm_arch}")
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_SUGGESTS "polkit")

set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
# /opt belongs to the filesystem package.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION "/opt")

# rpmbuild finds what the programs (not the libraries, which are not executable
# files) need: glibc and libstdc++ with the symbol versions the build used, which
# carry the floors, and the OpenGL libraries. The libraries of Qt and its xcb
# plug-in are listed here by soname, which every Red Hat and SUSE system answers
# with whatever its package for that library is called. They are the same
# libraries as the Debian list above.
set(CPACK_RPM_PACKAGE_REQUIRES
  "libbrotlidec.so.1()(64bit), libdbus-1.so.3()(64bit), libEGL.so.1()(64bit), libfontconfig.so.1()(64bit), libfreetype.so.6()(64bit), libGL.so.1()(64bit), libglib-2.0.so.0()(64bit), libgthread-2.0.so.0()(64bit), libgssapi_krb5.so.2()(64bit), libOpenGL.so.0()(64bit), libX11.so.6()(64bit), libX11-xcb.so.1()(64bit), libxcb-cursor.so.0()(64bit), libxcb-glx.so.0()(64bit), libxcb-icccm.so.4()(64bit), libxcb-image.so.0()(64bit), libxcb-keysyms.so.1()(64bit), libxcb-randr.so.0()(64bit), libxcb-render-util.so.0()(64bit), libxcb-render.so.0()(64bit), libxcb-shape.so.0()(64bit), libxcb-shm.so.0()(64bit), libxcb-sync.so.1()(64bit), libxcb-util.so.1()(64bit), libxcb-xfixes.so.0()(64bit), libxcb-xkb.so.1()(64bit), libxcb.so.1()(64bit), libxkbcommon-x11.so.0()(64bit), libxkbcommon.so.0()(64bit), libz.so.1()(64bit), libzstd.so.1()(64bit)")

# Two more things need saying to rpmbuild:
#   - the package must not offer the Qt and ICU libraries in
#     /opt/virtual-serial-studio to the system, or another program that needs
#     libQt6Core.so.6 would be given this private copy (the package still
#     provides its own name and its config marker, which dnf asks for);
#   - the programs ask for those libraries, and the package carries them
#     privately, so none must be asked for.
# Neither the debuginfo package nor the build id links are wanted for a tree the
# archive also holds.
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_SPEC_MORE_DEFINE "
%global __provides_exclude_from ^/opt/virtual-serial-studio/.*$
%global __requires_exclude ^(libQt6.*|libicu.*)[.]so.*$
%global debug_package %{nil}
%global _build_id_links none
%global __brp_check_rpaths %{nil}")
