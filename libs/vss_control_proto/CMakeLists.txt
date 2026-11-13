# SPDX-License-Identifier: GPL-3.0-or-later
#
# The control API between the gateway (vss_gate) and its clients: messages,
# errors, and the table that says who may do what. It does no I/O, so the
# decoder and the policy can be tested and fuzzed alone. The frame is the
# agent's. See docs/remote-control.md.
#

add_library(vss_control_proto
  src/error.cpp
  src/message.cpp
  src/policy.cpp)
add_library(Vss::control_proto ALIAS vss_control_proto)

target_include_directories(vss_control_proto PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(vss_control_proto PUBLIC cxx_std_23)
target_link_libraries(vss_control_proto
  PUBLIC vss_core vss_agent_proto
  PRIVATE Vss::BoostJson)

vss_set_warnings(vss_control_proto)
# boost::system::error_category deliberately has a non-virtual destructor and
# is meant to be derived from; GCC warns about the base anyway.
set_source_files_properties(src/error.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-non-virtual-dtor>")
vss_enable_coverage(vss_control_proto)
vss_mark_coverage_subject(vss_control_proto)
