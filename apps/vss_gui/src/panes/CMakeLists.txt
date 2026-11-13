# SPDX-License-Identifier: GPL-3.0-or-later
#
# Ports, Inspector, Send and Monitor, built on the backend and widget library.
#

add_library(vss_gui_panes STATIC
  fixture_behaviour.cpp
  fixture_behaviour.hpp
  fixture_panel.cpp
  fixture_panel.hpp
  format.hpp
  inspector_panel.cpp
  inspector_panel.hpp
  monitor_cell_delegate.cpp
  monitor_cell_delegate.hpp
  monitor_model.cpp
  monitor_model.hpp
  monitor_panel.cpp
  monitor_panel.hpp
  port_delegate.cpp
  port_delegate.hpp
  port_model.cpp
  port_model.hpp
  ports_panel.cpp
  ports_panel.hpp
  selection_service.hpp
  send_panel.cpp
  send_panel.hpp)

target_include_directories(vss_gui_panes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vss_gui_panes PUBLIC
  vss_gui_widgets
  vss_gui_backend
  vss_gui_theme
  Qt6::Widgets)

target_compile_features(vss_gui_panes PUBLIC cxx_std_23)
target_compile_definitions(vss_gui_panes PRIVATE QT_NO_KEYWORDS)
set_target_properties(vss_gui_panes PROPERTIES AUTOMOC ON)
vss_set_warnings(vss_gui_panes)

add_subdirectory(tests)
