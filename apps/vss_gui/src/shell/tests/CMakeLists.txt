# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shell tests: action registry, command palette, launcher and frame.
#

if(VSS_BUILD_TESTS)
  add_executable(vss_gui_shell_tests
    main.cpp
    test_responsiveness.cpp
    test_plugin_surfaces.cpp
    test_shell.cpp)

  target_include_directories(vss_gui_shell_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/framework)
  target_compile_definitions(vss_gui_shell_tests PRIVATE QT_NO_KEYWORDS)
  target_link_libraries(vss_gui_shell_tests PRIVATE
    vss_gui_shell
    Vss::studio
    Qt6::Widgets)
  vss_set_warnings(vss_gui_shell_tests)
endif()
