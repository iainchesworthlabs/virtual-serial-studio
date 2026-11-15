# SPDX-License-Identifier: GPL-3.0-or-later
#
# Update tests: the transport against a server on the loopback address.
#

if(VSS_BUILD_TESTS)
  add_executable(vss_gui_updates_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shell/tests/main.cpp
    test_network_transport.cpp)

  target_include_directories(vss_gui_updates_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/framework)
  target_compile_definitions(vss_gui_updates_tests PRIVATE QT_NO_KEYWORDS)
  target_link_libraries(vss_gui_updates_tests PRIVATE
    vss_gui_updates
    vss_gui_theme
    Qt6::Widgets)
  vss_set_warnings(vss_gui_updates_tests)
endif()
