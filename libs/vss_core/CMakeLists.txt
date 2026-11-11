# SPDX-License-Identifier: GPL-3.0-or-later
#
# Core topology engine: endpoints, links, routing, control lines and events.
#

add_library(vss_core
  src/bus_line.cpp
  src/bus_wire.cpp
  src/bytes.cpp
  src/control_lines.cpp
  src/delay_line.cpp
  src/endpoint.cpp
  src/error.cpp
  src/hub.cpp
  src/profiling.cpp
  src/serial_settings.cpp
  src/traffic_log.cpp
  src/types.cpp)
add_library(Vss::core ALIAS vss_core)

target_include_directories(vss_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(vss_core PUBLIC cxx_std_23)
target_link_libraries(vss_core PUBLIC Vss::Boost)

# The process environment is read and set per platform family; the Tracy side
# of vss::profiling exists only in an instrumented build.
target_include_directories(vss_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
  target_sources(vss_core PRIVATE src/platform/windows/environment_windows.cpp)
else()
  target_sources(vss_core PRIVATE src/platform/posix/environment_posix.cpp)
endif()

if(VSS_TRACY)
  # Public for in-tree consumers (they include profile.hpp), but not exported:
  # the installed headers do not require Tracy.
  target_link_libraries(vss_core PUBLIC $<BUILD_INTERFACE:Vss::Tracy>)
  target_sources(vss_core PRIVATE src/profiling_tracy.cpp)
  set(_vss_profiling_default_on "$<CONFIG:Debug>")
else()
  target_sources(vss_core PRIVATE src/profiling_off.cpp)
  set(_vss_profiling_default_on 0)
endif()
# 1 where the program starts profiling by itself (a Debug build with Tracy),
# 0 everywhere else.
target_compile_definitions(vss_core PRIVATE
  VSS_PROFILING_DEFAULT_ON=$<BOOL:${_vss_profiling_default_on}>)
unset(_vss_profiling_default_on)

find_package(Threads REQUIRED)
target_link_libraries(vss_core PUBLIC Threads::Threads)

if(WIN32)
  # Asio needs Winsock; MSVC auto-links it, clang-cl and MinGW do not.
  target_link_libraries(vss_core PUBLIC ws2_32 mswsock)
endif()

vss_set_warnings(vss_core)

# boost::system::error_category deliberately has a non-virtual destructor and
# is meant to be derived from; GCC warns about the base anyway.
set_source_files_properties(src/error.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-non-virtual-dtor>")
vss_enable_coverage(vss_core)
vss_mark_coverage_subject(vss_core)
