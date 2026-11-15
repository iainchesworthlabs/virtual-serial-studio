# SPDX-License-Identifier: GPL-3.0-or-later
#
# Data pane tests: models, formatting, filters and the Send/Ports behaviour.
#

if(VSS_BUILD_TESTS)
  add_executable(vss_gui_panes_tests
    main.cpp
    test_monitor.cpp
    test_panes.cpp)

  # The Monitor's column tests compile each catalogue with lrelease and
  # install it, to see that no heading is cut off in any language.
  find_package(Qt6 6.2 REQUIRED COMPONENTS LinguistTools)

  target_include_directories(vss_gui_panes_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/framework)
  target_compile_definitions(vss_gui_panes_tests PRIVATE QT_NO_KEYWORDS
    VSS_TRANSLATIONS_DIR="${CMAKE_SOURCE_DIR}/apps/vss_gui/translations"
    VSS_I18N_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    VSS_LRELEASE_EXE="$<TARGET_FILE:Qt6::lrelease>")
  target_link_libraries(vss_gui_panes_tests PRIVATE
    vss_gui_panes
    vss_gui_language
    Vss::studio
    Qt6::Widgets)
  vss_set_warnings(vss_gui_panes_tests)
endif()
