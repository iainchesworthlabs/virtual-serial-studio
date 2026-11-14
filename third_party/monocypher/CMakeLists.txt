# SPDX-License-Identifier: GPL-3.0-or-later
#
# Monocypher 4.0.2, unchanged (see README.md). Only the Ed25519 functions are
# used. The code is not ours, so it is built with its own warnings and is not
# a coverage subject.
#

add_library(vss_monocypher STATIC monocypher.c monocypher-ed25519.c)
add_library(Vss::Monocypher ALIAS vss_monocypher)
target_include_directories(vss_monocypher PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
set_target_properties(vss_monocypher PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
