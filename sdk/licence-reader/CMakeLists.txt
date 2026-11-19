# SPDX-License-Identifier: BSD-2-Clause
#
# The reference licence reader, built here so that Studio's own tests can run
# it against Studio's library. It is built as C++17, the oldest standard it
# supports, so a change that needs a newer one fails here. Nothing in Studio
# links it and it is in no package: a plugin author copies the two files.
#

add_library(vss_licence_reader STATIC licence_reader.cpp)
target_include_directories(vss_licence_reader PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(vss_licence_reader PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
target_link_libraries(vss_licence_reader PRIVATE Vss::Monocypher)
vss_set_warnings(vss_licence_reader)
vss_enable_coverage(vss_licence_reader)
