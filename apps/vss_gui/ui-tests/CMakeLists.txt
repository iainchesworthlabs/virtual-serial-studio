# SPDX-License-Identifier: GPL-3.0-or-later
#
# UI tests: journeys that drive the real window with QTest mouse and key
# events, a few at a time, as a person does. gui_journeys is the original
# handful; vss_gui_ui_tests is the suite described in docs/testing.md. Both use
# the in-tree framework; QTest supplies only the input simulation.
#

add_executable(vss_gui_journey_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/shell/tests/main.cpp
  test_journeys.cpp)

target_include_directories(vss_gui_journey_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/framework)
target_compile_definitions(vss_gui_journey_tests PRIVATE QT_NO_KEYWORDS)
target_link_libraries(vss_gui_journey_tests PRIVATE
  vss_gui_backend
  vss_gui_shell
  vss_gui_flows
  Vss::studio
  Qt6::Widgets
  Qt6::Test)
vss_set_warnings(vss_gui_journey_tests)

# The suite. One program, several CTest entries: each entry runs the journeys
# whose names start with one prefix (see tests/CMakeLists.txt).
# The platform's part of the harness (CPU time, crash hooks, fonts).
if(WIN32)
  set(VSS_UI_PLATFORM_SOURCE platform/ui_platform_windows.cpp)
else()
  set(VSS_UI_PLATFORM_SOURCE platform/ui_platform_posix.cpp)
endif()

add_executable(vss_gui_ui_tests
  ui_main.cpp
  ui_harness.cpp
  ${VSS_UI_PLATFORM_SOURCE}
  simple_steps.cpp
  advanced_steps.cpp
  test_simple_journeys.cpp
  test_advanced_journeys.cpp
  test_edge_journeys.cpp
  test_engine_journeys.cpp
  test_action_journeys.cpp
  test_setup_journeys.cpp)

target_include_directories(vss_gui_ui_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/framework
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(vss_gui_ui_tests PRIVATE QT_NO_KEYWORDS)
target_link_libraries(vss_gui_ui_tests PRIVATE
  vss_gui_backend
  vss_gui_shell
  vss_gui_flows
  Vss::studio
  Qt6::Widgets
  Qt6::Test)
# The stall budget of each journey, for a release build (docs/testing.md).
target_compile_definitions(vss_gui_ui_tests PRIVATE
  VSS_UI_BUDGETS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/perf_budgets.json")
vss_set_warnings(vss_gui_ui_tests)
