# SPDX-License-Identifier: GPL-3.0-or-later
#
# Design tokens, the rendered style sheet and the icon provider.  Everything
# that names a colour, font or size lives behind this target.
#

add_library(vss_gui_theme STATIC
  theme.cpp
  theme.hpp
  icon_provider.cpp
  icon_provider.hpp
  focus_ring_style.cpp
  focus_ring_style.hpp
  scaled_font.hpp
  theme_resources.qrc)

target_include_directories(vss_gui_theme PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(vss_gui_theme PUBLIC
  Vss::core
  Qt6::Widgets
  Qt6::Svg)

target_compile_features(vss_gui_theme PUBLIC cxx_std_23)
target_compile_definitions(vss_gui_theme PRIVATE QT_NO_KEYWORDS)
set_target_properties(vss_gui_theme PROPERTIES
  AUTOMOC ON
  AUTORCC ON)
vss_set_warnings(vss_gui_theme)
