# SPDX-License-Identifier: GPL-3.0-or-later
#
# Topology tests: layout, hit testing, keyboard flow, accessible text and the
# light/dark render.  They use a QApplication and the in-tree framework.
#

if(VSS_BUILD_TESTS)
  add_executable(vss_gui_topology_tests
    main.cpp
    test_topology.cpp
    test_link_curves.cpp
    test_canvas.cpp)

  target_include_directories(vss_gui_topology_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/framework)
  target_compile_definitions(vss_gui_topology_tests PRIVATE QT_NO_KEYWORDS)
  target_link_libraries(vss_gui_topology_tests PRIVATE
    vss_gui_topology
    vss_gui_shell
    Vss::studio
    Qt6::Widgets)
  vss_set_warnings(vss_gui_topology_tests)
endif()
