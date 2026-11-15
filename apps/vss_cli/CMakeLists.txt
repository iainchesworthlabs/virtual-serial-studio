# SPDX-License-Identifier: GPL-3.0-or-later
#
# vss_cli - headless access to the same studio the GUI drives.
#

# The command line parser and the JSON of `devices --json`, apart from the
# program so that the tests can call them.
add_library(vss_cli_core STATIC command_line.cpp devices_json.cpp)
target_compile_features(vss_cli_core PUBLIC cxx_std_23)
target_include_directories(vss_cli_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vss_cli_core PUBLIC Vss::studio PRIVATE Vss::BoostJson)
vss_set_warnings(vss_cli_core)

add_executable(vss_cli main.cpp agent_command.cpp control_command.cpp
  permissions_command.cpp plugin_command.cpp)
target_compile_features(vss_cli PRIVATE cxx_std_23)
target_include_directories(vss_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vss_cli PRIVATE vss_cli_core Vss::studio Vss::agent_core
  Vss::control_core Vss::plugin Vss::BoostJson vss_testkit)
target_compile_definitions(vss_cli PRIVATE
  VSS_CLI_VERSION="${PROJECT_VERSION}")
vss_set_warnings(vss_cli)

# What a closing terminal sends differs between systems; CMake chooses the
# source.
if(WIN32)
  target_sources(vss_cli PRIVATE
    platform/windows/stop_signals_windows.cpp)
elseif(UNIX)
  target_sources(vss_cli PRIVATE
    platform/posix/stop_signals_posix.cpp)
else()
  message(FATAL_ERROR "vss_cli: unsupported system ${CMAKE_SYSTEM_NAME}")
endif()

# The program's icon in Explorer, and its own description, in the Windows
# resource that cmake/VssWindowsResources.cmake makes (other platforms ignore
# the properties).
set_target_properties(vss_cli PROPERTIES
  VSS_ICON "${CMAKE_SOURCE_DIR}/apps/vss_gui/assets/icon/vss_gui.ico"
  VSS_FILE_DESCRIPTION "Virtual Serial Studio command line")

install(TARGETS vss_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT runtime)
