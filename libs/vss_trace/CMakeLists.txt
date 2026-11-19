# SPDX-License-Identifier: GPL-3.0-or-later
#
# The call recorder: a bounded in-memory ring of the operating system and
# library calls the transports make, with the raw code of each failure. It has
# no I/O and no dependency but Boost.System. See docs/debug-api.md.
#

add_library(vss_trace src/recorder.cpp)
add_library(Vss::trace ALIAS vss_trace)

target_include_directories(vss_trace PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(vss_trace PUBLIC cxx_std_23)
target_link_libraries(vss_trace PUBLIC Vss::Boost)

vss_set_warnings(vss_trace)
vss_enable_coverage(vss_trace)
vss_mark_coverage_subject(vss_trace)
