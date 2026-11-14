# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# Virtual Serial Studio
#
# A tool for real, virtual and networked serial ports, built on
# Boost.Asio and the Boost.VirtualSerial extension library.
#
#   libs/vss_core        topology, routing, control lines, events
#   libs/vss_ports       OS virtual ports (PTY, named pipe), serial transports
#   libs/vss_config      JSON configuration documents
#   libs/vss_rules       response timing rules: model, checks, matcher
#   libs/vss_studio      the application service used by the CLI and the GUI
#   libs/vss_testkit     reusable fakes for downstream tests
#   libs/vss_agent_proto the wire protocol of the per-user agent
#   libs/vss_agent       the agent's local transport, server and client
#   libs/vss_plugin      plugin manifests and the checks made before a plugin runs
#   libs/vss_control_proto the control API: messages, errors, the policy table
#   libs/vss_control     the control gateway, its audit log and its client
#   apps/vss_gate        the control gateway program
#   apps/                command line tool and Qt 6 GUI
#   tests/               unit, system and integration suites
#   perf/                throughput and latency floors
#   changelog.d/         one fragment per unreleased change

cmake_minimum_required(VERSION 3.21)

project(
  virtual_serial_studio
  VERSION 0.1.0
  DESCRIPTION "Tool for real, virtual and networked serial ports"
  HOMEPAGE_URL "https://github.com/iainchesworthlabs/virtual-serial-studio"
  LANGUAGES CXX)

# cmake/icl is a vendored copy of icl-common-infra/cmake (see cmake/icl/SOURCE
# and tools/sync_cmake.py); its FindQt6.cmake answers find_package(Qt6).
list(APPEND CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/icl")
set(ICL_PROJECT_PREFIX VSS)

include(GNUInstallDirs)

set(VSS_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(VSS_IS_TOP_LEVEL ON)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(VSS_BUILD_TESTS
  "Build the unit, system and integration test suites" ${VSS_IS_TOP_LEVEL})
option(VSS_BUILD_GUI
  "Build the Qt 6 configuration GUI when Qt is found" ON)
option(VSS_GUI_REQUIRED
  "Fail configure when the GUI is requested but Qt 6 is missing" OFF)
option(VSS_BUILD_PERF
  "Build the performance measurement suite" OFF)
option(VSS_BUILD_TOOLS
  "Build the auxiliary developer tools" ${VSS_IS_TOP_LEVEL})
option(VSS_COVERAGE
  "Instrument the libraries for statement and branch coverage" OFF)
# Tracy is compiled into every build that can find it, Debug and Release alike.
# Debug starts it by itself so a profiler can attach at any point; Release keeps
# it dormant (no thread, no socket) until Settings, --profile or VSS_PROFILE
# switches it on, so a problem seen in a release build can still be profiled.
# Without the vcpkg toolchain there is no package to find, so the default only
# applies when it is present.
set(_vss_tracy_default OFF)
if(DEFINED VCPKG_TARGET_TRIPLET)
  set(_vss_tracy_default ON)
endif()
option(VSS_TRACY
  "Instrument with Tracy: on by default in vcpkg builds" ${_vss_tracy_default})
unset(_vss_tracy_default)
option(VSS_INSTALL
  "Generate install and package configuration rules" ${VSS_IS_TOP_LEVEL})

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
    "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(MSVC)
  # The serial APIs need a Windows 7 baseline; NOMINMAX keeps std::min intact.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
endif()

# ---------------------------------------------------------------------------
# Dependencies and shared settings
# ---------------------------------------------------------------------------

include(IclPlatform)
include(VssWarnings)
include(VssVirtualSerial)
include(VssCoverage)
include(VssSanitizers)
include(VssTracy)
include(VssBuildInfo)
include(VssWindowsResources)
include(VssFuzz)

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------

# Monocypher (third_party) and the portable core of the Windows driver are C.
enable_language(C)
add_subdirectory(third_party/monocypher)
add_subdirectory(libs)

# The driver core is built for the tests only.
if(VSS_BUILD_TESTS)
  add_subdirectory(drivers/windows/vsscom/core)
endif()

if(VSS_BUILD_TOOLS)
  add_subdirectory(apps/vss_cli)
  add_subdirectory(apps/vss_agent)
  add_subdirectory(apps/vss_gate)
  add_subdirectory(apps/vss_license)
endif()

if(VSS_BUILD_GUI)
  add_subdirectory(apps/vss_gui)
endif()

if(VSS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(VSS_BUILD_PERF)
  add_subdirectory(perf)
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------

if(VSS_INSTALL)
  # Two components: "runtime" is what a release package holds (the programs,
  # the Qt libraries beside the GUI, the licence texts) and "development" is
  # the libraries and their headers.
  install(FILES LICENSE THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/virtual_serial_studio
    COMPONENT runtime)
  install(FILES LICENSES/BSL-1.0.txt LICENSES/GPL-3.0-or-later.txt
    LICENSES/BSD-2-Clause.txt LICENSES/CC0-1.0.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/virtual_serial_studio/LICENSES
    COMPONENT runtime)
  # The fonts are inside the program (Qt resources); their licences ship beside
  # the others.
  install(FILES
    apps/vss_gui/assets/fonts/IBMPlexMono-OFL.txt
    apps/vss_gui/assets/fonts/NotoSansArabic-OFL.txt
    apps/vss_gui/assets/fonts/NotoSansHebrew-OFL.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/virtual_serial_studio/LICENSES
    COMPONENT runtime)

  set(VSS_INSTALL_TARGETS "")
  foreach(_vss_target vss_core vss_ports vss_config vss_studio)
    if(TARGET ${_vss_target})
      list(APPEND VSS_INSTALL_TARGETS ${_vss_target})
      get_target_property(_vss_dir ${_vss_target} SOURCE_DIR)
      if(EXISTS "${_vss_dir}/include")
        install(DIRECTORY "${_vss_dir}/include/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
          COMPONENT development)
      endif()
    endif()
  endforeach()

  if(VSS_INSTALL_TARGETS)
    install(TARGETS ${VSS_INSTALL_TARGETS}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT development
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT development
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT development)
  endif()

  # Installers for the "runtime" component (CPack). The settings of each system
  # are in cmake/packaging/VssPackage-<system>.cmake, chosen by VssPackage.
  include(VssPackage)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "Virtual Serial Studio ${PROJECT_VERSION}")
message(STATUS "  C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Boost        : ${VSS_BOOST_INCLUDE_DIR}")
message(STATUS "  virtualserial: ${VSS_VIRTUAL_SERIAL_DIR}")
message(STATUS "  tests        : ${VSS_BUILD_TESTS}")
message(STATUS "  gui          : ${VSS_BUILD_GUI} (Qt found: ${VSS_QT_FOUND})")
message(STATUS "  perf         : ${VSS_BUILD_PERF}")
message(STATUS "  coverage     : ${VSS_COVERAGE}")
message(STATUS "")
