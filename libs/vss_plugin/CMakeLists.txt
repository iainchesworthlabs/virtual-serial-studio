# SPDX-License-Identifier: GPL-3.0-or-later
#
# Plugins: the manifest of a plugin, and the checks made on a plugin before
# any of its code runs. It starts no process and, apart from the load and save
# of the trust store, does no I/O, so it can be tested and fuzzed alone. See
# docs/plugins.md.
#

add_library(vss_plugin
  src/error.cpp
  src/hash.cpp
  src/manifest.cpp
  src/policy.cpp
  src/revocations.cpp
  src/trust_store.cpp
  src/verify.cpp)
add_library(Vss::plugin ALIAS vss_plugin)

target_include_directories(vss_plugin PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(vss_plugin PUBLIC cxx_std_23)
target_link_libraries(vss_plugin PUBLIC vss_core PRIVATE Vss::BoostJson)

vss_set_warnings(vss_plugin)
# boost::system::error_category deliberately has a non-virtual destructor and
# is meant to be derived from; GCC warns about the base anyway.
set_source_files_properties(src/error.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-non-virtual-dtor>")
vss_enable_coverage(vss_plugin)
vss_mark_coverage_subject(vss_plugin)
