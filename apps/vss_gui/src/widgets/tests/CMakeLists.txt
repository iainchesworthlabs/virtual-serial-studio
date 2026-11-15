# SPDX-License-Identifier: GPL-3.0-or-later
#
# Widget and theme tests.  They use a QApplication and the in-tree test
# framework, always on the offscreen platform.
#

if(VSS_BUILD_TESTS)
  add_executable(vss_gui_theme_tests
    main.cpp
    test_focus_ring.cpp
    test_theme.cpp
    test_widgets.cpp)

  target_include_directories(vss_gui_theme_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/framework)
  target_compile_definitions(vss_gui_theme_tests PRIVATE
    VSS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    QT_NO_KEYWORDS)
  target_link_libraries(vss_gui_theme_tests PRIVATE
    vss_gui_widgets
    Vss::studio
    Qt6::Widgets)
  vss_set_warnings(vss_gui_theme_tests)
endif()
