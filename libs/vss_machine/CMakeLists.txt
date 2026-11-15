# SPDX-License-Identifier: GPL-3.0-or-later
#
# What this computer says: the product's two data folders, the computer's
# name, and who can write to a path.  The licence store and the plugin
# library both rest on it.  It depends on Boost.System only: no Qt, no other
# library of this project.
#
# The operating system is asked through the files in
# src/platform/<windows|linux|macos>, which CMake picks.  Linux and macOS
# share one implementation of the access check.
#

add_library(vss_machine
  src/access.cpp
  src/computer_name.cpp)
add_library(Vss::machine ALIAS vss_machine)

target_include_directories(vss_machine PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(vss_machine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(vss_machine PUBLIC cxx_std_23)
target_link_libraries(vss_machine PUBLIC Vss::Boost)

icl_platform_sources(vss_machine)
if(UNIX)
  target_sources(vss_machine PRIVATE src/platform/posix/access_posix.cpp)
endif()
if(WIN32)
  # advapi32 for owners and access lists; shell32 and ole32 for the folders
  # Windows names (ProgramData, LocalAppData).
  target_link_libraries(vss_machine PRIVATE advapi32 shell32 ole32)
endif()
if(APPLE)
  # The name in System Settings, Sharing, is the system configuration's.
  find_library(VSS_MACHINE_CORE_FOUNDATION CoreFoundation REQUIRED)
  find_library(VSS_MACHINE_SYSTEM_CONFIGURATION SystemConfiguration REQUIRED)
  target_link_libraries(vss_machine PRIVATE
    ${VSS_MACHINE_CORE_FOUNDATION} ${VSS_MACHINE_SYSTEM_CONFIGURATION})
endif()

vss_set_warnings(vss_machine)
vss_enable_coverage(vss_machine)
vss_mark_coverage_subject(vss_machine)
