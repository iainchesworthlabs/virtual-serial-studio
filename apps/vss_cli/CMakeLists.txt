# SPDX-License-Identifier: GPL-3.0-or-later
#
# vss_cli - headless access to the same studio the GUI drives.
#

add_executable(vss_cli main.cpp agent_command.cpp control_command.cpp
  permissions_command.cpp)
target_compile_features(vss_cli PRIVATE cxx_std_23)
target_link_libraries(vss_cli PRIVATE Vss::studio Vss::agent_core
  Vss::control_core vss_testkit)
vss_set_warnings(vss_cli)

# The program's icon in Explorer, and its own description, in the Windows
# resource that cmake/VssWindowsResources.cmake makes (other platforms ignore
# the properties).
set_target_properties(vss_cli PROPERTIES
  VSS_ICON "${CMAKE_SOURCE_DIR}/apps/vss_gui/assets/icon/vss_gui.ico"
  VSS_FILE_DESCRIPTION "Virtual Serial Studio command line")

install(TARGETS vss_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT runtime)
