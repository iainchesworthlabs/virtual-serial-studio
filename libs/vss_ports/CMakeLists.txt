# SPDX-License-Identifier: GPL-3.0-or-later
#
# Transports: real serial through Boost.VirtualSerial, OS virtual ports,
# control lines and serial device enumeration.
#

add_library(vss_ports
  src/com0com.cpp
  src/driver_setup.cpp
  src/listen_transport.cpp
  src/loopback_transport.cpp
  src/native_lines.cpp
  src/peer_open.cpp
  src/permissions.cpp
  src/rfc2217.cpp
  src/serial_enumeration.cpp
  src/serial_transport.cpp
  src/stable_link.cpp
  src/vcom_driver.cpp
  src/virtual_port.cpp)
add_library(Vss::ports ALIAS vss_ports)

target_include_directories(vss_ports PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(vss_ports PUBLIC cxx_std_23)
target_link_libraries(vss_ports PUBLIC vss_core Vss::VirtualSerial)
icl_platform_sources(vss_ports)

# Linux and macOS share these. Windows has its own files in src/platform/windows.
if(UNIX)
  target_sources(vss_ports PRIVATE
    src/platform/posix/com0com_posix.cpp
    src/platform/posix/native_lines_posix.cpp
    src/platform/posix/process_control_posix.cpp
    src/platform/posix/serial_enumeration_posix.cpp
    src/platform/posix/vcom_driver_posix.cpp
    src/platform/posix/virtual_port_posix.cpp)
endif()

if(WIN32)
  target_link_libraries(vss_ports PRIVATE setupapi shell32 advapi32 ws2_32 mswsock
    ole32 oleaut32)
  # public.h of the driver (the interface GUID, control codes and the hub state)
  # and the protocol of the helper service.
  target_include_directories(vss_ports PRIVATE
    ${PROJECT_SOURCE_DIR}/drivers/windows)
endif()

vss_set_warnings(vss_ports)
vss_enable_coverage(vss_ports)
vss_mark_coverage_subject(vss_ports)
