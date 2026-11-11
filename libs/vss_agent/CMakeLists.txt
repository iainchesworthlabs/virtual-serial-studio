# SPDX-License-Identifier: GPL-3.0-or-later
#
# The per-user agent: the local transport (a named pipe on Windows, a Unix
# domain socket elsewhere), the server that owns the connections to one
# studio, and the client the command line tool uses. See docs/agent.md.
#

add_library(vss_agent_core
  src/client.cpp
  src/local_stream.cpp
  src/server.cpp)
add_library(Vss::agent_core ALIAS vss_agent_core)

target_include_directories(vss_agent_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(vss_agent_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(vss_agent_core PUBLIC cxx_std_23)
target_link_libraries(vss_agent_core PUBLIC vss_agent_proto vss_studio)
target_compile_definitions(vss_agent_core PRIVATE
  VSS_AGENT_VERSION="${PROJECT_VERSION}")
icl_platform_sources(vss_agent_core)

# Linux and macOS share the socket code; the one call that differs (who is at
# the other end of a socket) is in src/platform/linux and src/platform/macos.
if(UNIX)
  target_sources(vss_agent_core PRIVATE
    src/platform/posix/local_stream_posix.cpp
    src/platform/posix/process_posix.cpp)
endif()

if(WIN32)
  target_link_libraries(vss_agent_core PRIVATE advapi32)
endif()

vss_set_warnings(vss_agent_core)
vss_enable_coverage(vss_agent_core)
vss_mark_coverage_subject(vss_agent_core)
