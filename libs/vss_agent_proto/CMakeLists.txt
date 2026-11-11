# SPDX-License-Identifier: GPL-3.0-or-later
#
# The wire protocol between vss_agent and its clients: frames, messages and
# the errors that cross the connection. It does no I/O, so the decoder can be
# tested and fuzzed alone. See docs/agent.md.
#

add_library(vss_agent_proto
  src/error.cpp
  src/frame.cpp
  src/message.cpp)
add_library(Vss::agent_proto ALIAS vss_agent_proto)

target_include_directories(vss_agent_proto PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(vss_agent_proto PUBLIC cxx_std_23)
target_link_libraries(vss_agent_proto PUBLIC vss_core PRIVATE Vss::BoostJson)

vss_set_warnings(vss_agent_proto)
# boost::system::error_category deliberately has a non-virtual destructor and
# is meant to be derived from; GCC warns about the base anyway.
set_source_files_properties(src/error.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-non-virtual-dtor>")
vss_enable_coverage(vss_agent_proto)
vss_mark_coverage_subject(vss_agent_proto)
