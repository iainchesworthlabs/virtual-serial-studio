# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit and system suites.  Both use the in-tree framework in framework/.
#

set(VSS_UNIT_TEST_SOURCES
  framework/main.cpp
  unit/test_bytes.cpp
  unit/test_control_lines.cpp
  unit/test_document.cpp
  unit/test_driver_setup.cpp
  unit/test_endpoint.cpp
  unit/test_endpoint_reopen.cpp
  unit/test_endpoint_status.cpp
  unit/test_error_and_log.cpp
  unit/test_hub_bus.cpp
  unit/test_hub_events.cpp
  unit/test_hub_latency.cpp
  unit/test_hub_topology.cpp
  unit/test_listen_transport.cpp
  unit/test_loopback_transport.cpp
  unit/test_rfc2217.cpp
  unit/test_native_and_com0com.cpp
  unit/test_permissions.cpp
  unit/test_permissions_linux.cpp
  unit/test_permissions_macos.cpp
  unit/test_profiling.cpp
  unit/test_serial_settings.cpp
  unit/test_serial_transport.cpp
  unit/test_stable_link.cpp
  unit/test_benchmark.cpp
  unit/test_capture.cpp
  unit/test_replay.cpp
  unit/test_responder.cpp
  unit/test_studio.cpp
  unit/test_studio_reopen.cpp
  unit/test_studio_links.cpp
  unit/test_virtual_ports.cpp
  unit/test_vsscom_core.cpp)

# Cases that need a platform API or expect platform-specific results live in
# unit/platform/<os>/ and are compiled only there.
if(WIN32)
  list(APPEND VSS_UNIT_TEST_SOURCES
    unit/platform/windows/test_driver_client_windows.cpp
    unit/platform/windows/test_helper_windows.cpp
    unit/platform/windows/test_manifest_windows.cpp
    unit/platform/windows/test_ports_windows.cpp)
elseif(UNIX)
  list(APPEND VSS_UNIT_TEST_SOURCES unit/platform/posix/test_ports_posix.cpp
    unit/platform/posix/test_permissions_posix.cpp)
endif()

add_executable(vss_unit_tests ${VSS_UNIT_TEST_SOURCES})
target_include_directories(vss_unit_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/framework
  ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(vss_unit_tests PRIVATE vss_studio vss_testkit vsscom_core)
if(WIN32)
  # public.h of the driver and the helper protocol, for the protocol tests.
  target_include_directories(vss_unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/drivers/windows)
endif()
target_compile_definitions(vss_unit_tests PRIVATE
  VSS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
vss_set_warnings(vss_unit_tests)
vss_enable_coverage(vss_unit_tests)

add_test(NAME unit COMMAND vss_unit_tests
  --verbose --junit=${CMAKE_BINARY_DIR}/unit-tests.xml)
set_tests_properties(unit PROPERTIES TIMEOUT 300)

set(VSS_SYSTEM_TEST_SOURCES
  framework/main.cpp
  system/test_system_bus.cpp
  system/test_system_listen.cpp
  system/test_system_serial.cpp
  system/test_system_tcp.cpp
  system/test_system_udp.cpp)

# The vport tests start vss_cli as a separate process, the way a script does.
if(NOT WIN32 AND TARGET vss_cli)
  list(APPEND VSS_SYSTEM_TEST_SOURCES system/test_system_vport.cpp)
endif()

add_executable(vss_system_tests ${VSS_SYSTEM_TEST_SOURCES})
target_include_directories(vss_system_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/framework
  ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(vss_system_tests PRIVATE vss_studio vss_testkit)
vss_set_warnings(vss_system_tests)
vss_enable_coverage(vss_system_tests)

add_test(NAME system COMMAND vss_system_tests
  --verbose --junit=${CMAKE_BINARY_DIR}/system-tests.xml)
set_tests_properties(system PROPERTIES TIMEOUT 600)
if(NOT WIN32 AND TARGET vss_cli)
  add_dependencies(vss_system_tests vss_cli)
  target_compile_definitions(vss_system_tests PRIVATE
    VSS_CLI_PATH="$<TARGET_FILE:vss_cli>")
endif()

add_subdirectory(fuzz)

# A packaging smoke test: the public headers compile on their own.
add_executable(vss_header_selftest compile/header_selftest.cpp)
target_link_libraries(vss_header_selftest PRIVATE vss_studio vss_testkit)
vss_set_warnings(vss_header_selftest)
add_test(NAME headers COMMAND vss_header_selftest)

# The VM harness is checked against a stand-in for vmrun, so no VM is needed.
# PowerShell 7 is on every hosted runner and optional elsewhere.
find_program(VSS_PWSH NAMES pwsh)
if(VSS_PWSH)
  add_test(NAME vm_harness
    COMMAND ${VSS_PWSH} -NoProfile -File
            ${PROJECT_SOURCE_DIR}/tools/vm/test-vmrun.ps1)
  set_tests_properties(vm_harness PROPERTIES TIMEOUT 120)

  # The VMX builder runs in its dry-run mode. The answer disc is built with IMAPI2,
  # so its test is for Windows only.
  add_test(NAME vm_builder
    COMMAND ${VSS_PWSH} -NoProfile -File
            ${PROJECT_SOURCE_DIR}/tools/vm/test-vm.ps1)
  set_tests_properties(vm_builder PROPERTIES TIMEOUT 120)
  add_test(NAME vm_guest_cell
    COMMAND ${VSS_PWSH} -NoProfile -File
            ${PROJECT_SOURCE_DIR}/tools/vm/test-guest-cell.ps1)
  set_tests_properties(vm_guest_cell PROPERTIES TIMEOUT 180)
  add_test(NAME vm_credentials
    COMMAND ${VSS_PWSH} -NoProfile -File
            ${PROJECT_SOURCE_DIR}/tools/vm/test-credentials.ps1)
  set_tests_properties(vm_credentials PROPERTIES TIMEOUT 120)
  if(WIN32)
    add_test(NAME vm_seed
      COMMAND ${VSS_PWSH} -NoProfile -File
              ${PROJECT_SOURCE_DIR}/tools/vm/test-seed.ps1)
    set_tests_properties(vm_seed PROPERTIES TIMEOUT 180)
  endif()
endif()

# A sanitizer build runs several times slower than a plain one (ThreadSanitizer five to fifteen
# times), so the fixed per-test timeouts below would fail a run that is only slow. At the end of
# this directory, whatever was added above or by a later change, every test's TIMEOUT is multiplied.
set(VSS_SANITIZER_TIMEOUT_FACTOR 5 CACHE STRING
  "Multiplier for the test timeouts in a build with ICL_SANITIZERS")
function(vss_scale_test_timeouts)
  get_directory_property(_vss_tests TESTS)
  foreach(_vss_test IN LISTS _vss_tests)
    get_test_property(${_vss_test} TIMEOUT _vss_timeout)
    if(_vss_timeout)
      math(EXPR _vss_timeout "${_vss_timeout} * ${VSS_SANITIZER_TIMEOUT_FACTOR}")
      set_tests_properties(${_vss_test} PROPERTIES TIMEOUT ${_vss_timeout})
    endif()
  endforeach()
endfunction()
if(ICL_SANITIZERS)
  cmake_language(DEFER CALL vss_scale_test_timeouts)
endif()

# The GUI tests set PATH=${VSS_QT_BIN_DIR};$ENV{PATH}, but ENVIRONMENT is a
# list, so the semicolon ends the entry and PATH is the Qt directory alone. That is
# enough for Qt, and not for the MSVC AddressSanitizer runtime DLL, which lives in
# the compiler's directory. Put that directory in the same entry, with the
# semicolon escaped so that it stays inside it.
if(MSVC AND ICL_SANITIZERS AND VSS_QT_BIN_DIR)
  get_filename_component(_vss_asan_dll_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  set(VSS_QT_BIN_DIR "${VSS_QT_BIN_DIR}\\;${_vss_asan_dll_dir}")
endif()

if(TARGET vss_gui)
  # The offscreen plugin needs an explicit font directory on Windows; the
  # clipping walk cannot measure text without one.
  set(VSS_QT_FONT_ENV "")
  if(WIN32 AND EXISTS "C:/Windows/Fonts")
    set(VSS_QT_FONT_ENV "QT_QPA_FONTDIR=C:/Windows/Fonts;")
  endif()
  add_test(NAME gui_selftest COMMAND vss_gui --selftest)
  set_tests_properties(gui_selftest PROPERTIES
    TIMEOUT 240
    ENVIRONMENT "${VSS_QT_FONT_ENV}QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")

  # The same walk at 200 percent scale: text has to grow without clipping and
  # every control must stay reachable.  Qt applies the scale before the first
  # window exists, which a runtime font change cannot do for token fonts.
  add_test(NAME gui_selftest_200 COMMAND vss_gui --selftest)
  set_tests_properties(gui_selftest_200 PROPERTIES
    TIMEOUT 300
    ENVIRONMENT "${VSS_QT_FONT_ENV}QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2;QT_FONT_DPI=192;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

# The backend tests exist only when Qt and the tests are both enabled.
if(TARGET vss_gui_backend_tests)
  add_test(NAME gui_backend COMMAND vss_gui_backend_tests --verbose)
  set_tests_properties(gui_backend PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

# Language choice and layout direction.
if(TARGET vss_gui_language_tests)
  add_test(NAME gui_language COMMAND vss_gui_language_tests --verbose)
  set_tests_properties(gui_language PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

# Journey tests send key and mouse events to the real window.
if(TARGET vss_gui_journey_tests)
  add_test(NAME gui_journeys COMMAND vss_gui_journey_tests --verbose)
  set_tests_properties(gui_journeys PROPERTIES
    LABELS ui
    RUN_SERIAL TRUE
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

# The UI suite: journeys driven only by simulated input (docs/testing.md).
# One program, one CTest entry per group, so each has its own timeout and can
# be run alone. RUN_SERIAL keeps CTest from starting any other test beside a
# GUI test: on Windows the settings, the clipboard and the window station are
# shared between processes, and a GUI test that waits for a window needs the
# machine quiet. The programs keep their QSettings in a directory of their own.
# VSS_UI_BUDGET_MS is how long the GUI thread may be blocked inside one step.
if(TARGET vss_gui_ui_tests)
  set(VSS_UI_ENV
    "${VSS_QT_FONT_ENV}QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
  set(VSS_UI_ENV
    "${VSS_UI_ENV};VSS_UI_FAILURE_DIR=${CMAKE_BINARY_DIR}/ui-test-failures")
  # The budgets in ui-tests/perf_budgets.json are for a release build. A debug
  # build, or one with sanitizers or coverage, is slower by a known factor, so
  # the budget is multiplied by it. A loose budget still fails a journey that
  # freezes.
  if(ICL_SANITIZERS MATCHES "thread")
    set(VSS_UI_BUDGET_SCALE 25)
  elseif(ICL_SANITIZERS)
    set(VSS_UI_BUDGET_SCALE 6)
  elseif(VSS_COVERAGE)
    set(VSS_UI_BUDGET_SCALE 60)
  elseif(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(VSS_UI_BUDGET_SCALE 1)
  else()
    set(VSS_UI_BUDGET_SCALE 12)
  endif()
  set(VSS_UI_ENV "${VSS_UI_ENV};VSS_UI_BUDGET_SCALE=${VSS_UI_BUDGET_SCALE}")
  foreach(group simple advanced edge actions engine setup)
    add_test(NAME ui_${group}
      COMMAND vss_gui_ui_tests --verbose --filter=${group}:)
    set(_labels ui ui-${group})
    set_tests_properties(ui_${group} PROPERTIES
      LABELS "${_labels}"
      TIMEOUT 900
      RUN_SERIAL TRUE
      ENVIRONMENT
        "${VSS_UI_ENV};VSS_UI_PERF_REPORT=${CMAKE_BINARY_DIR}/ui-test-perf/${group}.csv")
  endforeach()
  # The edge cases again at 200 percent scale, where text is twice as large.
  add_test(NAME ui_edge_200
    COMMAND vss_gui_ui_tests --verbose --filter=edge:)
  set_tests_properties(ui_edge_200 PROPERTIES
    LABELS "ui;ui-edge"
    TIMEOUT 900
    RUN_SERIAL TRUE
    ENVIRONMENT
      "${VSS_UI_ENV};QT_SCALE_FACTOR=2;QT_FONT_DPI=192;VSS_UI_PERF_REPORT=${CMAKE_BINARY_DIR}/ui-test-perf/edge-200.csv")
endif()

# Theme and widget tests need a QApplication but stay offscreen.
if(TARGET vss_gui_theme_tests)
  add_test(NAME gui_theme COMMAND vss_gui_theme_tests --verbose)
  set_tests_properties(gui_theme PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")

  # The same tests at 200 percent scale, where icons were once clipped.
  add_test(NAME gui_theme_200 COMMAND vss_gui_theme_tests --verbose)
  set_tests_properties(gui_theme_200 PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

if(TARGET vss_gui_shell_tests)
  add_test(NAME gui_shell COMMAND vss_gui_shell_tests --verbose)
  set_tests_properties(gui_shell PROPERTIES
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

# Simple mode: setups, job forms and setup pages.
if(TARGET vss_gui_simple_tests)
  add_test(NAME gui_simple COMMAND vss_gui_simple_tests --verbose)
  set_tests_properties(gui_simple PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

# Ports, Inspector, Send and Monitor models and panels.
if(TARGET vss_gui_panes_tests)
  add_test(NAME gui_panes COMMAND vss_gui_panes_tests --verbose)
  set_tests_properties(gui_panes PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

if(TARGET vss_gui_topology_tests)
  add_test(NAME gui_topology COMMAND vss_gui_topology_tests --verbose)
  set_tests_properties(gui_topology PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

# Advanced tools need a QApplication; every case runs offscreen.
if(TARGET vss_gui_advanced_tests)
  add_test(NAME gui_advanced COMMAND vss_gui_advanced_tests --verbose)
  set_tests_properties(gui_advanced PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

if(TARGET vss_gui_support_tests)
  add_test(NAME gui_support COMMAND vss_gui_support_tests --verbose)
  set_tests_properties(gui_support PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

# Connect, share, network and template dialogs (WP6).
if(TARGET vss_gui_dialogs_tests)
  add_test(NAME gui_dialogs COMMAND vss_gui_dialogs_tests --verbose)
  set_tests_properties(gui_dialogs PROPERTIES
    TIMEOUT 240
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()
if(TARGET vss_gui_flows_tests)
  add_test(NAME gui_flows COMMAND vss_gui_flows_tests --verbose)
  set_tests_properties(gui_flows PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${VSS_QT_BIN_DIR};$ENV{PATH}")
endif()

# The matrix tools are checked without a built program; Python is optional.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME matrix_tools
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${PROJECT_SOURCE_DIR}/tools/matrix -p "test_*.py")
  set_tests_properties(matrix_tools PROPERTIES TIMEOUT 120)
endif()

# vss_gui must belong to the Windows subsystem: a console-subsystem program gets
# a console window whenever it is not started from a terminal. This reads the
# PE header, so it needs no desktop session.
if(WIN32 AND TARGET vss_gui AND Python3_FOUND)
  add_test(NAME gui_subsystem
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/check_pe_subsystem.py
            $<TARGET_FILE:vss_gui> windows)
  set_tests_properties(gui_subsystem PROPERTIES TIMEOUT 30)
endif()
