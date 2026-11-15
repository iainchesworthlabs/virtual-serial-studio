# SPDX-License-Identifier: GPL-3.0-or-later
#
# The plugin host: what runs a plugin once libs/vss_plugin has checked it.
# So far it holds one thing, a plugin as a process of the operating system:
# started with a private channel and nothing else of the host's, its output
# captured, and ended together with everything it started. There is no
# sandbox yet, and nothing in Studio starts a plugin. See docs/plugins.md.
#

add_library(vss_plugin_host
  src/bundle_sections.cpp
  src/decode_store.cpp
  src/process.cpp)
add_library(Vss::plugin_host ALIAS vss_plugin_host)

target_include_directories(vss_plugin_host PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(vss_plugin_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(vss_plugin_host PUBLIC cxx_std_23)
target_link_libraries(vss_plugin_host PUBLIC vss_core Vss::plugin)

# How a process is started differs by system. CMake picks the file; Linux
# and macOS share one.
if(WIN32)
  target_sources(vss_plugin_host PRIVATE
    src/platform/windows/process_windows.cpp)
  # advapi32 for the user's SID and the pipe's access list; bcrypt for the
  # random part of a pipe's name.
  target_link_libraries(vss_plugin_host PRIVATE advapi32 bcrypt)
else()
  target_sources(vss_plugin_host PRIVATE
    src/platform/posix/process_posix.cpp)
endif()

vss_set_warnings(vss_plugin_host)
vss_enable_coverage(vss_plugin_host)
vss_mark_coverage_subject(vss_plugin_host)
