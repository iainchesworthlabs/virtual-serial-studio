# SPDX-License-Identifier: GPL-3.0-or-later
#
# vss_agent - the per-user program that owns the studio, so that ports outlive
# the window and the command that made them. See docs/agent.md.
#
# It is not installed yet: the installers are not changed until the design in
# docs/agent.md is agreed.
#

add_executable(vss_agent main.cpp)
target_compile_features(vss_agent PRIVATE cxx_std_23)
target_include_directories(vss_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vss_agent PRIVATE Vss::agent_core)

# CMake chooses the platform source.
if(WIN32)
  target_sources(vss_agent PRIVATE
    src/platform/windows/stop_signals_windows.cpp)
elseif(UNIX)
  target_sources(vss_agent PRIVATE
    src/platform/posix/stop_signals_posix.cpp)
else()
  message(FATAL_ERROR "vss_agent: unsupported system ${CMAKE_SYSTEM_NAME}")
endif()

vss_set_warnings(vss_agent)

set_target_properties(vss_agent PROPERTIES
  VSS_ICON "${CMAKE_SOURCE_DIR}/apps/vss_gui/assets/icon/vss_gui.ico"
  VSS_FILE_DESCRIPTION "Virtual Serial Studio agent")
