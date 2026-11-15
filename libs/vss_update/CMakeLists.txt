# SPDX-License-Identifier: GPL-3.0-or-later
#
# Online updates: semantic versions, the signed update feed and the choice of
# what to offer (docs/updates.md). No Qt, no network and no file I/O: the
# program that fetches the feed hands its text to this library.
#

add_library(vss_update
  src/error.cpp
  src/feed.cpp
  src/select.cpp
  src/strict_json.cpp
  src/trusted_keys_release.cpp
  src/version.cpp)
add_library(Vss::update ALIAS vss_update)

target_include_directories(vss_update PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(vss_update PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(vss_update PUBLIC cxx_std_23)
# The signed envelope and the Ed25519 check are the licence library's; the
# feed is that envelope with a purpose of its own, and its key type is in the
# interface of this one.
target_link_libraries(vss_update
  PUBLIC Vss::license
  PRIVATE Vss::BoostJson)

vss_set_warnings(vss_update)
# boost::system::error_category deliberately has a non-virtual destructor and
# is meant to be derived from; GCC warns about the base anyway.
set_source_files_properties(src/error.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-non-virtual-dtor>")
vss_enable_coverage(vss_update)
vss_mark_coverage_subject(vss_update)
