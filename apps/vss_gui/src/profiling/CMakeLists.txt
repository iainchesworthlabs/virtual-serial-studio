# SPDX-License-Identifier: GPL-3.0-or-later
#
# Profiling support for the GUI: the QApplication that records a Tracy zone for
# every event it dispatches, the event-loop and paint frames, and the
# controller that the Settings page, the status bar and About use to switch
# the profiler on and off. Everything here is dormant until the profiler is
# started; see docs/profiling.md.
#

add_library(vss_gui_profiling STATIC
  profiled_application.cpp
  profiled_application.hpp
  profiling_controller.cpp
  profiling_controller.hpp
  stall_watchdog.cpp
  stall_watchdog.hpp)

target_include_directories(vss_gui_profiling PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vss_gui_profiling PUBLIC Vss::core Qt6::Widgets)
target_compile_features(vss_gui_profiling PUBLIC cxx_std_23)
target_compile_definitions(vss_gui_profiling PRIVATE QT_NO_KEYWORDS)
set_target_properties(vss_gui_profiling PROPERTIES AUTOMOC ON)
vss_set_warnings(vss_gui_profiling)
