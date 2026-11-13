# SPDX-License-Identifier: GPL-3.0-or-later
#
# The topology dock: a QGraphicsView over the studio snapshot, its layout and
# its optional links list.  Presentation only; it never calls the studio.
#

add_library(vss_gui_topology STATIC
  topology_layout.cpp
  topology_layout.hpp
  link_curve.cpp
  link_curve.hpp
  node_item.cpp
  node_item.hpp
  link_item.cpp
  link_item.hpp
  topology_text.cpp
  topology_text.hpp
  topology_view.cpp
  topology_view.hpp
  links_list.cpp
  links_list.hpp
  toolbox.cpp
  toolbox.hpp
  add_sheet.cpp
  add_sheet.hpp)

target_include_directories(vss_gui_topology PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(vss_gui_topology PUBLIC
  vss_gui_backend
  vss_gui_rendering
  vss_gui_theme
  vss_gui_widgets
  Qt6::Widgets)

target_compile_features(vss_gui_topology PUBLIC cxx_std_23)
target_compile_definitions(vss_gui_topology PRIVATE QT_NO_KEYWORDS)
set_target_properties(vss_gui_topology PROPERTIES AUTOMOC ON)
vss_set_warnings(vss_gui_topology)

add_subdirectory(tests)
