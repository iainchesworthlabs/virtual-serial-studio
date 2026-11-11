# SPDX-License-Identifier: GPL-3.0-or-later
#
# The shared widget library.  Every widget reads colours, type and metrics
# from vss_gui_theme.
#

add_library(vss_gui_widgets STATIC
  common.hpp
  chips.cpp
  chips.hpp
  notes.cpp
  notes.hpp
  controls.cpp
  controls.hpp
  dialogs.cpp
  dialogs.hpp
  port_menu.cpp
  port_menu.hpp
  gallery.cpp
  gallery.hpp
  half_duplex_fields.cpp
  half_duplex_fields.hpp
  pick_row.cpp
  pick_row.hpp
  surfaces.cpp
  surfaces.hpp)

target_link_libraries(vss_gui_widgets PUBLIC vss_gui_theme)

target_compile_features(vss_gui_widgets PUBLIC cxx_std_23)
target_compile_definitions(vss_gui_widgets PRIVATE QT_NO_KEYWORDS)
set_target_properties(vss_gui_widgets PROPERTIES AUTOMOC ON)
vss_set_warnings(vss_gui_widgets)

add_subdirectory(tests)
