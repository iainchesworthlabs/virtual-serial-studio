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

# `vss_cli control ...` is a library so that the unit tests can run the command
# in their own process, against a gateway of their own.
add_library(vss_cli_control STATIC control_command.cpp)
target_include_directories(vss_cli_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vss_cli_control PUBLIC cxx_std_23)
target_link_libraries(vss_cli_control PUBLIC Vss::control_core Vss::agent_core)
vss_set_warnings(vss_cli_control)

# `vss_cli debug ...` is a library too, so that the unit tests can run it in
# their own process, against a debug server of their own.
add_library(vss_cli_debug STATIC debug_command.cpp)
target_include_directories(vss_cli_debug PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vss_cli_debug PUBLIC cxx_std_23)
target_link_libraries(vss_cli_debug PUBLIC Vss::debug PRIVATE Vss::BoostJson)
vss_set_warnings(vss_cli_debug)

# The licence command is a library of its own so that the unit tests run it in
# their own process, with licence folders they made.
add_library(vss_cli_licence STATIC licence_command.cpp)
target_include_directories(vss_cli_licence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vss_cli_licence PUBLIC cxx_std_23)
target_link_libraries(vss_cli_licence PUBLIC Vss::license)
vss_set_warnings(vss_cli_licence)

add_executable(vss_cli main.cpp agent_command.cpp permissions_command.cpp
  plugin_command.cpp plugin_install_command.cpp plugin_run_command.cpp
  plugin_support.cpp)
target_compile_features(vss_cli PRIVATE cxx_std_23)
target_include_directories(vss_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vss_cli PRIVATE vss_cli_core vss_cli_control
  vss_cli_debug vss_cli_licence Vss::studio
  Vss::agent_core Vss::control_core Vss::plugin Vss::plugin_host Vss::machine
  Vss::BoostJson vss_testkit)
vss_set_warnings(vss_cli)

# The build identity, stamped before every build, as the window has it.
vss_add_build_info(vss_cli
  TEMPLATE ${CMAKE_CURRENT_SOURCE_DIR}/build_version.cpp.in
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/build_version.cpp)

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
