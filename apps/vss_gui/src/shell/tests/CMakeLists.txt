# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shell tests: action registry, command palette, launcher and frame.
#

if(VSS_BUILD_TESTS)
  add_executable(vss_gui_shell_tests
    main.cpp
    test_responsiveness.cpp
    test_plugin_surfaces.cpp
    test_shell.cpp
    test_update_shell.cpp)

  # The update screens are driven by the update service, with the fakes of the
  # service's own tests and the throwaway key those tests sign feeds with.
  target_include_directories(vss_gui_shell_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/framework
    ${CMAKE_SOURCE_DIR}/apps/vss_gui/src/updates/tests)
  target_compile_definitions(vss_gui_shell_tests PRIVATE QT_NO_KEYWORDS)
  target_link_libraries(vss_gui_shell_tests PRIVATE
    vss_gui_shell
    Vss::core
    Vss::license_test_keys
    Vss::studio
    Qt6::Widgets)
  vss_set_warnings(vss_gui_shell_tests)
endif()
