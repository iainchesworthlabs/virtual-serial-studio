# SPDX-License-Identifier: GPL-3.0-or-later
#
# The support and system surfaces: setup check, troubleshooting, the
# diagnostic bundle, settings, About, keyboard shortcuts and the help index.
#

add_library(vss_gui_support STATIC
  about_dialog.cpp
  about_dialog.hpp
  build_info.cpp
  build_info.hpp
  bundle.cpp
  bundle.hpp
  check_row.cpp
  check_row.hpp
  console.hpp
  diagnostic_bundle_dialog.cpp
  diagnostic_bundle_dialog.hpp
  driver_setup_dialog.cpp
  driver_setup_dialog.hpp
  elevated_step.cpp
  elevated_step.hpp
  event_log.cpp
  event_log.hpp
  firewall_dialog.cpp
  firewall_dialog.hpp
  help_contents_dialog.cpp
  help_contents_dialog.hpp
  permission_dialog.cpp
  permission_dialog.hpp
  permission_rows.cpp
  permission_rows.hpp
  redact.cpp
  redact.hpp
  running_setup.cpp
  running_setup.hpp
  settings.cpp
  settings.hpp
  settings_dialog.cpp
  settings_dialog.hpp
  setup_check_dialog.cpp
  setup_check_dialog.hpp
  setup_facts.cpp
  setup_facts.hpp
  shortcuts_dialog.cpp
  sign_in_start.cpp
  sign_in_start.hpp
  stall_probe.cpp
  stall_probe.hpp
  shortcuts_dialog.hpp
  troubleshoot.cpp
  troubleshoot.hpp
  troubleshoot_dialog.cpp
  troubleshoot_dialog.hpp
  zip_writer.cpp
  zip_writer.hpp)

# The sign-in entry, one file per platform.
icl_platform_sources(vss_gui_support
  DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform)

target_include_directories(vss_gui_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(vss_gui_support PUBLIC
  vss_gui_language
  vss_gui_widgets
  vss_gui_backend
  vss_gui_profiling
  Qt6::Widgets)

if(WIN32)
  target_link_libraries(vss_gui_support PRIVATE shell32)
endif()

target_compile_features(vss_gui_support PUBLIC cxx_std_23)
target_compile_definitions(vss_gui_support PRIVATE QT_NO_KEYWORDS)
target_compile_definitions(vss_gui_support PUBLIC
  VSS_GIT_COMMIT="${VSS_GIT_COMMIT}"
  VSS_GIT_BRANCH="${VSS_GIT_BRANCH}"
  VSS_BUILD_DATE="${VSS_BUILD_DATE}"
  VSS_BUILD_TYPE="${VSS_BUILD_TYPE}"
  VSS_COMPILER="${VSS_COMPILER}"
  VSS_GUI_VERSION="${PROJECT_VERSION}")
set_target_properties(vss_gui_support PROPERTIES AUTOMOC ON)
vss_set_warnings(vss_gui_support)

add_subdirectory(tests)
