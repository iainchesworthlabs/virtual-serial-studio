# SPDX-License-Identifier: GPL-3.0-or-later
#
# Licence files and the signed envelope they share with plugin manifests
# (docs/licensing.md). No Qt and no network code.
#
#   vss_license            verify and judge; what Studio and plugins link
#   vss_license_sign       signing; the issuing tool and the tests only
#   vss_license_test_keys  the throwaway RFC 8032 key; never in a product
#

add_library(vss_license
  src/error.cpp
  src/license.cpp
  src/signed_file.cpp
  src/strict_json.cpp
  src/trusted_keys_release.cpp)
add_library(Vss::license ALIAS vss_license)

target_include_directories(vss_license PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(vss_license PUBLIC cxx_std_23)
target_link_libraries(vss_license
  PUBLIC Vss::Boost
  PRIVATE Vss::BoostJson Vss::Monocypher)

vss_set_warnings(vss_license)
# boost::system::error_category deliberately has a non-virtual destructor and
# is meant to be derived from; GCC warns about the base anyway.
set_source_files_properties(src/error.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-non-virtual-dtor>")
vss_enable_coverage(vss_license)
vss_mark_coverage_subject(vss_license)

add_library(vss_license_sign src/sign.cpp)
add_library(Vss::license_sign ALIAS vss_license_sign)
target_link_libraries(vss_license_sign
  PUBLIC vss_license
  PRIVATE Vss::Monocypher)
vss_set_warnings(vss_license_sign)
vss_enable_coverage(vss_license_sign)
vss_mark_coverage_subject(vss_license_sign)

add_library(vss_license_test_keys src/test_keys.cpp)
add_library(Vss::license_test_keys ALIAS vss_license_test_keys)
target_link_libraries(vss_license_test_keys PUBLIC vss_license_sign)
vss_set_warnings(vss_license_test_keys)
