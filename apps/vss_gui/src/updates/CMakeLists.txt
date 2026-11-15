# SPDX-License-Identifier: GPL-3.0-or-later
#
# Updates: the service the window reads for everything about updates
# (docs/updates.md), the transport that makes the request, and the files
# that differ by system (the kind of install, the checked file and the
# hand-over to the system). No widgets. The service is given its transport,
# its clock and its folders, so its tests make no request.
#

add_library(vss_gui_updates STATIC
  checked_file.hpp
  feed_transport.hpp
  install_kind.cpp
  install_kind.hpp
  network_transport.cpp
  network_transport.hpp
  package_install.cpp
  package_install.hpp
  plain_checked_file.cpp
  plain_checked_file.hpp
  update_service.cpp
  update_service.hpp
  update_types.hpp)

# The kind of install, the checked file and the package step, one file per
# platform.
icl_platform_sources(vss_gui_updates
  DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform)

target_include_directories(vss_gui_updates PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(vss_gui_updates PUBLIC
  Vss::update
  Qt6::Core
  Qt6::Gui
  Qt6::Network)

if(WIN32)
  target_link_libraries(vss_gui_updates PRIVATE shell32)
endif()

target_compile_features(vss_gui_updates PUBLIC cxx_std_23)
target_compile_definitions(vss_gui_updates PRIVATE QT_NO_KEYWORDS)
set_target_properties(vss_gui_updates PROPERTIES AUTOMOC ON)
vss_set_warnings(vss_gui_updates)

add_subdirectory(tests)
