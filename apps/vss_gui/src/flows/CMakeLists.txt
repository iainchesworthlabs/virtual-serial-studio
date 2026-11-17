# SPDX-License-Identifier: GPL-3.0-or-later
#
# The WP5 basic flows: Add a port, Port ready, Connect two programs, Attach a
# real device and Watch traffic.  Screens call the backend adapter only.
#

add_library(vss_gui_flows STATIC
  common.cpp
  common.hpp
  com_number_dialog.cpp
  com_number_dialog.hpp
  flow_dialog.cpp
  flow_dialog.hpp
  add_port_dialog.cpp
  add_port_dialog.hpp
  pair_panel.cpp
  pair_panel.hpp
  pair_dialog.cpp
  pair_dialog.hpp
  port_ready_dialog.cpp
  port_ready_dialog.hpp
  attach_dialog.cpp
  attach_dialog.hpp
  decode_dialog.cpp
  decode_dialog.hpp
  watch_dialog.cpp
  watch_dialog.hpp
  rename_dialog.cpp
  rename_dialog.hpp
  release_port_dialog.cpp
  release_port_dialog.hpp)

target_include_directories(vss_gui_flows PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(vss_gui_flows PUBLIC
  vss_gui_backend
  vss_gui_widgets
  vss_gui_theme
  vss_gui_support
  Qt6::Widgets)

target_compile_features(vss_gui_flows PUBLIC cxx_std_23)
target_compile_definitions(vss_gui_flows PRIVATE QT_NO_KEYWORDS)
set_target_properties(vss_gui_flows PROPERTIES AUTOMOC ON)
vss_set_warnings(vss_gui_flows)

if(VSS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
