# SPDX-License-Identifier: GPL-3.0-or-later
#
# Support tests: setup check, troubleshooting, support bundle, settings,
# About, shortcuts and the confirm dialog.
#

if(VSS_BUILD_TESTS)
  add_executable(vss_gui_support_tests
    main.cpp
    test_app_identity.cpp
    test_bundle.cpp
    test_driver_setup.cpp
    test_licence.cpp
    test_permission_dialog.cpp
    test_permission_rows.cpp
    test_plugin_prompts.cpp
    test_plugins_page.cpp
    test_profiling.cpp
    test_render_settings.cpp
    test_rendering.cpp
    test_running.cpp
    test_support.cpp
    test_support_dialogs.cpp
    test_update_dialog.cpp
    test_update_screens.cpp
    test_update_text.cpp)

  # The update screens are driven by the update service, with the fakes of the
  # service's own tests and the throwaway key those tests sign feeds with.
  target_include_directories(vss_gui_support_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/framework
    ${CMAKE_SOURCE_DIR}/apps/vss_gui/src/updates/tests)
  target_compile_definitions(vss_gui_support_tests PRIVATE QT_NO_KEYWORDS
    VSS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  target_link_libraries(vss_gui_support_tests PRIVATE
    vss_gui_support
    Vss::core
    Vss::license_test_keys
    Vss::studio
    Qt6::Widgets)
  vss_set_warnings(vss_gui_support_tests)
endif()
