# SPDX-License-Identifier: GPL-3.0-or-later
#
# The control endpoint: the gateway that serves the control API over this
# user's local pipe or socket and is a client of the agent, its audit log, and
# the client the command line tool uses. Nothing here opens a network
# listener. See docs/remote-control.md.
#

add_library(vss_control_core
  src/audit_log.cpp
  src/client.cpp
  src/failure_limiter.cpp
  src/gateway.cpp
  src/junit.cpp
  src/junit.cpp
  src/token_file.cpp
  src/token_store.cpp)
add_library(Vss::control_core ALIAS vss_control_core)

target_include_directories(vss_control_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(vss_control_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(vss_control_core PUBLIC cxx_std_23)
target_link_libraries(vss_control_core
  PUBLIC vss_control_proto vss_agent_core
  PRIVATE Vss::BoostJson)
target_compile_definitions(vss_control_core PRIVATE
  VSS_CONTROL_VERSION="${PROJECT_VERSION}")
icl_platform_sources(vss_control_core)

# Linux and macOS share the code for private files; where a program keeps its
# state is in src/platform/linux and src/platform/macos.
if(UNIX)
  target_sources(vss_control_core PRIVATE
    src/platform/posix/private_storage_posix.cpp)
endif()

if(WIN32)
  target_link_libraries(vss_control_core PRIVATE advapi32 bcrypt shell32 ole32)
endif()

vss_set_warnings(vss_control_core)
vss_enable_coverage(vss_control_core)
vss_mark_coverage_subject(vss_control_core)
