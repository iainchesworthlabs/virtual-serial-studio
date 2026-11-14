# SPDX-License-Identifier: GPL-3.0-or-later
#
# vss_license - reads, checks and issues licence files (docs/licensing.md).
# It is the owner's tool and is not installed with Studio. It links the test
# key for its --test mode; Studio never links that part.
#
#   vss_license_tool  the commands, which the unit tests also run
#   vss_license_program  the program, built as vss_license
#

add_library(vss_license_tool STATIC tool.cpp)
target_include_directories(vss_license_tool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vss_license_tool PUBLIC cxx_std_23)
target_link_libraries(vss_license_tool
  PUBLIC Vss::license
  PRIVATE Vss::license_sign Vss::license_test_keys Vss::BoostJson)

# The random source, the owner-only key file and the environment differ by
# system. CMake picks the file; the code has no platform tests.
if(WIN32)
  target_sources(vss_license_tool PRIVATE platform/windows/platform_windows.cpp)
  target_link_libraries(vss_license_tool PRIVATE bcrypt advapi32)
else()
  target_sources(vss_license_tool PRIVATE platform/posix/platform_posix.cpp)
endif()

vss_set_warnings(vss_license_tool)
vss_enable_coverage(vss_license_tool)

# The library is the target vss_license, so the program's target has another
# name. The file it builds is vss_license.
add_executable(vss_license_program main.cpp)
target_link_libraries(vss_license_program PRIVATE vss_license_tool)
vss_set_warnings(vss_license_program)
set_target_properties(vss_license_program PROPERTIES
  OUTPUT_NAME vss_license
  VSS_FILE_DESCRIPTION "Virtual Serial Studio licence tool")
