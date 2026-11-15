# SPDX-License-Identifier: GPL-3.0-or-later
#
# The plugin host: what runs a plugin once libs/vss_plugin has checked it.
# So far it holds one thing, a plugin as a process of the operating system:
# started with a private channel and nothing else of the host's, its output
# captured, and ended together with everything it started. There is no
# sandbox yet, and nothing in Studio starts a plugin. See docs/plugins.md.
#

add_library(vss_plugin_host
  src/control_relay.cpp
  src/generator_feed.cpp
  src/instances.cpp
  src/process.cpp
  src/runner.cpp
  src/sandbox.cpp
  src/service.cpp
  src/start_plan.cpp
  src/traffic_tap.cpp)
add_library(Vss::plugin_host ALIAS vss_plugin_host)

target_include_directories(vss_plugin_host PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(vss_plugin_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(vss_plugin_host PUBLIC cxx_std_23)
target_link_libraries(vss_plugin_host PUBLIC vss_core Vss::plugin)
# The control relay speaks to the gateway as the gateway's own clients do: the
# local transport and frames of the agent, and the messages of the control API.
target_link_libraries(vss_plugin_host PRIVATE vss_agent_core vss_control_proto
  Vss::BoostJson)

# How a process is started differs by system. CMake picks the file; Linux
# and macOS share one.
if(WIN32)
  target_sources(vss_plugin_host PRIVATE
    src/platform/windows/process_windows.cpp
    src/platform/windows/stream_windows.cpp)
  # advapi32 for the user's SID and the pipe's access list; bcrypt for the
  # random part of a pipe's name.
  target_link_libraries(vss_plugin_host PRIVATE advapi32 bcrypt userenv)
else()
  target_sources(vss_plugin_host PRIVATE
    src/platform/posix/process_posix.cpp
    src/platform/posix/stream_posix.cpp)
endif()

vss_set_warnings(vss_plugin_host)
vss_enable_coverage(vss_plugin_host)
vss_mark_coverage_subject(vss_plugin_host)
