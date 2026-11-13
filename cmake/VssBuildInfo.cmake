# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# vss_add_build_info(<target> TEMPLATE <file.cpp.in> OUTPUT <file.cpp>)
#
# Adds OUTPUT to <target>, generated from TEMPLATE before every build by
# VssBuildStamp.cmake (version, tag, commit, branch, dirty flag, CI build
# number, build time, build type and compiler). No preprocessor definitions
# are involved: the values live in a generated source file.
#
# VSS_BUILD_NUMBER overrides the number the CI run supplies; VSS_VERSION_TAG
# overrides the tag, for a build outside git or outside CI.

set(VSS_BUILD_NUMBER "" CACHE STRING
  "Build number to show in About (default: the GitHub Actions run number)")
set(VSS_VERSION_TAG "" CACHE STRING
  "Release tag to take the version from, for example v1.2.3 (default: git)")

find_package(Git QUIET)

function(vss_add_build_info target)
  cmake_parse_arguments(ARG "" "TEMPLATE;OUTPUT" "" ${ARGN})
  set(stamp_target ${target}_build_stamp)
  add_custom_target(${stamp_target}
    COMMAND ${CMAKE_COMMAND}
      "-DIN=${ARG_TEMPLATE}"
      "-DOUT=${ARG_OUTPUT}"
      "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      "-DPROJECT_VERSION=${PROJECT_VERSION}"
      "-DBUILD_TYPE=$<CONFIG>"
      "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
      "-DBUILD_NUMBER=${VSS_BUILD_NUMBER}"
      "-DVERSION_OVERRIDE=${VSS_VERSION_TAG}"
      -P "${CMAKE_SOURCE_DIR}/cmake/VssBuildStamp.cmake"
    BYPRODUCTS "${ARG_OUTPUT}" "${ARG_OUTPUT}.state"
    COMMENT "Stamping the build information"
    VERBATIM)
  set_source_files_properties("${ARG_OUTPUT}" PROPERTIES GENERATED TRUE)
  target_sources(${target} PRIVATE "${ARG_OUTPUT}")
  add_dependencies(${target} ${stamp_target})
  set_target_properties(${stamp_target} PROPERTIES FOLDER "build-info")
endfunction()
