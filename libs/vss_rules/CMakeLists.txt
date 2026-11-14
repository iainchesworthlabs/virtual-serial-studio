# SPDX-License-Identifier: GPL-3.0-or-later
#
# Response timing rules: the rule set a harness writes, its checks, the
# compiled form and the machine that says what fires when. It has no hub, no
# clock and no threads, so it is tested with times written in the tests and
# its reader is fuzzed alone. See docs/response-timing.md.
#

add_library(vss_rules
  src/compile.cpp
  src/machine.cpp
  src/parse.cpp
  src/random.cpp
  src/scheduler.cpp)
add_library(Vss::rules ALIAS vss_rules)

target_include_directories(vss_rules PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(vss_rules PUBLIC cxx_std_23)
target_link_libraries(vss_rules PUBLIC vss_core PRIVATE Vss::BoostJson)

vss_set_warnings(vss_rules)
vss_enable_coverage(vss_rules)
vss_mark_coverage_subject(vss_rules)
