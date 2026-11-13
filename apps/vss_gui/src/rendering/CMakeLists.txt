# SPDX-License-Identifier: GPL-3.0-or-later
#
# How the canvas is drawn: the choice between the GPU and the CPU, the GPU
# canvas (Qt Canvas Painter on QRhi) and the GPU zones for the profiler. See
# docs/gui-design/docs/rendering.md.
#

# Tracy's GPU zones are compiled only into a build that has Tracy; the other
# build gets stand-ins with the same interface.
if(VSS_TRACY)
  set(_vss_gpu_profiler gpu_profiler_tracy.cpp)
else()
  set(_vss_gpu_profiler gpu_profiler_off.cpp)
endif()

add_library(vss_gui_rendering STATIC
  render_mode.cpp
  render_mode.hpp
  render_state.cpp
  render_state.hpp
  gpu_canvas.cpp
  gpu_canvas.hpp
  gpu_profiler.hpp
  ${_vss_gpu_profiler})
unset(_vss_gpu_profiler)

target_include_directories(vss_gui_rendering PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vss_gui_rendering PUBLIC
  Vss::core Qt6::Widgets Qt6::CanvasPainter Qt6::GuiPrivate)
target_compile_features(vss_gui_rendering PUBLIC cxx_std_23)
target_compile_definitions(vss_gui_rendering PRIVATE QT_NO_KEYWORDS)
set_target_properties(vss_gui_rendering PROPERTIES AUTOMOC ON)
vss_set_warnings(vss_gui_rendering)

