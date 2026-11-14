# SPDX-License-Identifier: GPL-3.0-or-later
#
# The application service shared by the CLI and the Qt GUI.
#

add_library(vss_studio src/studio.cpp src/reconnect.cpp src/format_check.cpp src/benchmark.cpp src/capture.cpp src/replay.cpp src/responder.cpp src/studio_rules.cpp)
add_library(Vss::studio ALIAS vss_studio)

target_include_directories(vss_studio PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(vss_studio PUBLIC cxx_std_23)
target_link_libraries(vss_studio PUBLIC vss_core vss_ports vss_config vss_rules)

vss_set_warnings(vss_studio)
vss_enable_coverage(vss_studio)
vss_mark_coverage_subject(vss_studio)
