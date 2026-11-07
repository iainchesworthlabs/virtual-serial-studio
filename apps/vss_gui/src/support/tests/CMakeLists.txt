# SPDX-License-Identifier: GPL-3.0-or-later
#
# Support tests: setup check, troubleshooting, diagnostic bundle, settings,
# About, shortcuts and the confirm dialog.
#

if(VSS_BUILD_TESTS)
  add_executable(vss_gui_support_tests
    main.cpp
    test_driver_setup.cpp
    test_permission_dialog.cpp
    test_permission_rows.cpp
    test_profiling.cpp
    test_running.cpp
    test_support.cpp)

  target_include_directories(vss_gui_support_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/framework)
  target_compile_definitions(vss_gui_support_tests PRIVATE QT_NO_KEYWORDS)
  target_link_libraries(vss_gui_support_tests PRIVATE
    vss_gui_support
    Vss::studio
    Qt6::Widgets)
  vss_set_warnings(vss_gui_support_tests)
endif()
