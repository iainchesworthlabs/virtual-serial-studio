# SPDX-License-Identifier: GPL-3.0-or-later
#
# Plugins: the manifest of a plugin, and the checks made on a plugin before
# any of its code runs. It starts no process. Its I/O is the load and save of
# the trust store, the reading of a plugin's directory and of the policy file,
# and asking who may write to a file; the rest works on bytes it is given, so
# it can be tested and fuzzed alone. See docs/plugins.md.
#

add_library(vss_plugin
  src/access.cpp
  src/discover.cpp
  src/error.cpp
  src/hash.cpp
  src/locations.cpp
  src/manifest.cpp
  src/measure.cpp
  src/policy.cpp
  src/revocations.cpp
  src/trust_store.cpp
  src/verify.cpp)
add_library(Vss::plugin ALIAS vss_plugin)

target_include_directories(vss_plugin PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(vss_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(vss_plugin PUBLIC cxx_std_23)
target_link_libraries(vss_plugin PUBLIC vss_core PRIVATE Vss::BoostJson)

# Where the plugin folders and the policy file are, and who owns a file and
# who may write to it, are asked of the operating system.  CMake picks the
# files in src/platform/<windows|linux|macos>.  Linux and macOS share one
# implementation of the access check.
icl_platform_sources(vss_plugin)
if(UNIX)
  target_sources(vss_plugin PRIVATE src/platform/posix/access_posix.cpp)
endif()
if(WIN32)
  # advapi32 for owners and access lists; shell32 and ole32 for the folders
  # Windows names (ProgramData, LocalAppData).
  target_link_libraries(vss_plugin PRIVATE advapi32 shell32 ole32)
endif()

vss_set_warnings(vss_plugin)
# boost::system::error_category deliberately has a non-virtual destructor and
# is meant to be derived from; GCC warns about the base anyway.
set_source_files_properties(src/error.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-non-virtual-dtor>")
vss_enable_coverage(vss_plugin)
vss_mark_coverage_subject(vss_plugin)
