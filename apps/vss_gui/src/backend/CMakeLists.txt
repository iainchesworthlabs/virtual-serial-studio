# SPDX-License-Identifier: GPL-3.0-or-later
#
# The GUI backend: the adapter between every screen and vss::studio, plus the
# coalescing event bridge.  Kept in a separate static library so the tests can
# link it without Qt Widgets.
#

add_library(vss_gui_backend STATIC
  studio_backend.cpp
  engine_studio_backend.cpp
  fake_studio_backend.cpp
  event_bridge.cpp
  background_work.cpp
  share_edit.cpp
  canvas_edit.cpp
  com_numbers.cpp
  driver_error.cpp
  licence_store.cpp
  licence_store.hpp
  licence_view.hpp
  studio_backend.hpp
  engine_studio_backend.hpp
  fake_studio_backend.hpp
  event_bridge.hpp
  background_work.hpp
  share_edit.hpp
  canvas_edit.hpp
  com_numbers.hpp
  driver_error.hpp
  capabilities.hpp
  other_ports.hpp
  view_types.hpp)

target_include_directories(vss_gui_backend PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(vss_gui_backend PUBLIC Vss::studio Vss::license Qt6::Core)
target_compile_features(vss_gui_backend PUBLIC cxx_std_23)
target_compile_definitions(vss_gui_backend PRIVATE QT_NO_KEYWORDS)
icl_platform_sources(vss_gui_backend
  DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform)
set_target_properties(vss_gui_backend PROPERTIES AUTOMOC ON)
vss_set_warnings(vss_gui_backend)

# boost::system::error_category deliberately has a non-virtual destructor and
# is meant to be derived from; GCC warns about the base anyway (the same
# exception libs/vss_core makes for its own category).
set_source_files_properties(driver_error.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-non-virtual-dtor>")

if(VSS_BUILD_TESTS)
  add_executable(vss_gui_backend_tests
    tests/main.cpp
    tests/test_fake_backend.cpp
    tests/test_event_bridge.cpp
    tests/test_background_work.cpp
    tests/test_share_edit.cpp
    tests/test_canvas_edit.cpp
    tests/test_com_numbers.cpp
    tests/test_driver_error.cpp
    tests/test_licence_store.cpp)

  target_include_directories(vss_gui_backend_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/framework)
  # The licence tests sign with the published test key. The program itself
  # never links it.
  target_link_libraries(vss_gui_backend_tests PRIVATE
    vss_gui_backend Vss::license_test_keys Qt6::Core)
  target_compile_definitions(vss_gui_backend_tests PRIVATE QT_NO_KEYWORDS)
  vss_set_warnings(vss_gui_backend_tests)
endif()
