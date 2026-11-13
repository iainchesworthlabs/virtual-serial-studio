# SPDX-License-Identifier: GPL-3.0-or-later
#
# vss_gate - the control gateway: it serves the control API to this user's
# programs over a local pipe or socket, and is itself a client of the agent.
# See docs/remote-control.md.
#
# It is not installed yet: the installers are not changed until the design in
# docs/remote-control.md is built.
#

add_executable(vss_gate main.cpp)
target_compile_features(vss_gate PRIVATE cxx_std_23)
target_include_directories(vss_gate PRIVATE
  ${CMAKE_SOURCE_DIR}/apps/vss_agent/src)
target_link_libraries(vss_gate PRIVATE Vss::control_core)

# The gateway stops on the same signals as the agent, so it compiles the
# agent's own files for them. CMake chooses the platform source.
if(WIN32)
  target_sources(vss_gate PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/vss_agent/src/platform/windows/stop_signals_windows.cpp)
elseif(UNIX)
  target_sources(vss_gate PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/vss_agent/src/platform/posix/stop_signals_posix.cpp)
else()
  message(FATAL_ERROR "vss_gate: unsupported system ${CMAKE_SYSTEM_NAME}")
endif()

vss_set_warnings(vss_gate)

set_target_properties(vss_gate PROPERTIES
  VSS_ICON "${CMAKE_SOURCE_DIR}/apps/vss_gui/assets/icon/vss_gui.ico"
  VSS_FILE_DESCRIPTION "Virtual Serial Studio control gateway")
