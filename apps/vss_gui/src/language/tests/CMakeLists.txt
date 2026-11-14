# SPDX-License-Identifier: GPL-3.0-or-later
#
# Language choice: which catalogue a list of system languages selects, and
# which languages are written right to left.
#

if(VSS_BUILD_TESTS)
  add_executable(vss_gui_language_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shell/tests/main.cpp
    test_bidi.cpp
    test_language_choice.cpp
    test_translations.cpp)

  find_package(Qt6 6.2 REQUIRED COMPONENTS LinguistTools)
  # The catalogue test runs lupdate over the same sources the build gives it
  # (see apps/vss_gui/CMakeLists.txt) and compares the result with the .ts files.
  file(GLOB_RECURSE VSS_I18N_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/apps/vss_gui/main.cpp"
    "${CMAKE_SOURCE_DIR}/apps/vss_gui/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/apps/vss_gui/src/*.hpp")
  list(FILTER VSS_I18N_SOURCES EXCLUDE REGEX "/tests/")
  list(JOIN VSS_I18N_SOURCES "\n" VSS_I18N_SOURCE_TEXT)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/i18n_sources.txt"
    "${VSS_I18N_SOURCE_TEXT}\n")

  target_include_directories(vss_gui_language_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/framework)
  target_compile_definitions(vss_gui_language_tests PRIVATE QT_NO_KEYWORDS
    VSS_TRANSLATIONS_DIR="${CMAKE_SOURCE_DIR}/apps/vss_gui/translations"
    VSS_I18N_SOURCE_LIST="${CMAKE_CURRENT_BINARY_DIR}/i18n_sources.txt"
    VSS_I18N_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    VSS_LUPDATE_EXE="$<TARGET_FILE:Qt6::lupdate>"
    VSS_LRELEASE_EXE="$<TARGET_FILE:Qt6::lrelease>")
  target_link_libraries(vss_gui_language_tests PRIVATE
    vss_gui_language
    vss_gui_theme
    Vss::studio
    Qt6::Widgets)
  vss_set_warnings(vss_gui_language_tests)
endif()
