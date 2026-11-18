# SPDX-License-Identifier: GPL-3.0-or-later
#
# The debug API of a running Studio: the server that answers over this user's
# local pipe or socket while the person has debug access on, the studio
# provider that fills in the answers, the redaction every reply goes through,
# and the client `vss_cli debug` uses. Nothing here opens a network listener.
# See docs/debug-api.md.
#

add_library(vss_debug
  src/client.cpp
  src/json_support.cpp
  src/protocol.cpp
  src/provider.cpp
  src/redact.cpp
  src/server.cpp
  src/studio_provider.cpp
  src/trace_text.cpp)
add_library(Vss::debug ALIAS vss_debug)

target_include_directories(vss_debug PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(vss_debug PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(vss_debug PUBLIC cxx_std_23)
target_link_libraries(vss_debug
  PUBLIC vss_trace vss_studio vss_agent_core vss_control_core
  PRIVATE Vss::BoostJson)

vss_set_warnings(vss_debug)
vss_enable_coverage(vss_debug)
vss_mark_coverage_subject(vss_debug)
