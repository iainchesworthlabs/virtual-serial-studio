# SPDX-License-Identifier: GPL-3.0-or-later
#
# The update service: every state of a check and of a download, driven with a
# fake transport, a fake clock and a fake probe. Those tests make no request.
# The transport is tested against a server on the loopback address.
#

if(VSS_BUILD_TESTS)
  add_executable(vss_gui_updates_tests
    main.cpp
    test_network_transport.cpp
    test_update_check.cpp
    test_update_download.cpp
    test_update_platform.cpp
    test_update_timer.cpp)

  target_include_directories(vss_gui_updates_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/framework)
  # The tests sign feeds with the published test key. The program itself
  # never links it.
  target_link_libraries(vss_gui_updates_tests PRIVATE
    vss_gui_updates Vss::core Vss::license_test_keys Qt6::Core Qt6::Network)
  target_compile_definitions(vss_gui_updates_tests PRIVATE QT_NO_KEYWORDS)
  vss_set_warnings(vss_gui_updates_tests)
endif()
