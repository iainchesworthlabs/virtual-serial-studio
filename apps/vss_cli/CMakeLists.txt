# SPDX-License-Identifier: GPL-3.0-or-later
#
# vss_cli - headless access to the same studio the GUI drives.
#

# The licence command is a library of its own so that the unit tests run it in
# their own process, with licence folders they made.
add_library(vss_cli_licence STATIC licence_command.cpp)
target_include_directories(vss_cli_licence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vss_cli_licence PUBLIC cxx_std_23)
target_link_libraries(vss_cli_licence PUBLIC Vss::license)
vss_set_warnings(vss_cli_licence)

add_executable(vss_cli main.cpp agent_command.cpp control_command.cpp
  permissions_command.cpp plugin_command.cpp)
target_compile_features(vss_cli PRIVATE cxx_std_23)
target_link_libraries(vss_cli PRIVATE Vss::studio Vss::agent_core
  Vss::control_core Vss::plugin Vss::BoostJson vss_testkit vss_cli_licence)
vss_set_warnings(vss_cli)

# The program's icon in Explorer, and its own description, in the Windows
# resource that cmake/VssWindowsResources.cmake makes (other platforms ignore
# the properties).
set_target_properties(vss_cli PROPERTIES
  VSS_ICON "${CMAKE_SOURCE_DIR}/apps/vss_gui/assets/icon/vss_gui.ico"
  VSS_FILE_DESCRIPTION "Virtual Serial Studio command line")

install(TARGETS vss_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT runtime)
