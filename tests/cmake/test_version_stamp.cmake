# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# Checks the version string that every build carries: how
# vss_compose_version() writes it, and what cmake/VssBuildStamp.cmake makes of
# a real git history (no tag, on a tag, after a tag, with a modified tree).
#
#   cmake -DSOURCE_ROOT=<repository> [-DGIT_EXECUTABLE=<git> -DWORK_DIR=<dir>]
#         -P test_version_stamp.cmake
#
# The part with git needs both GIT_EXECUTABLE and WORK_DIR, and makes a
# throwaway repository in WORK_DIR.

if(NOT SOURCE_ROOT)
  message(FATAL_ERROR "Give -DSOURCE_ROOT=<the repository>")
endif()

include("${SOURCE_ROOT}/cmake/VssVersion.cmake")

function(expect actual expected what)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR "${what}: expected \"${expected}\", got \"${actual}\"")
  endif()
endfunction()

# --- the composition --------------------------------------------------------

vss_compose_version(text VERSION 0.1.0-rc.5)
expect("${text}" "0.1.0-rc.5" "the release alone")

vss_compose_version(text VERSION 0.1.0-rc.5 DISTANCE 18 COMMIT 7d42b4e)
expect("${text}" "0.1.0-rc.5+18.g7d42b4e" "after the tag")

vss_compose_version(text VERSION 0.1.0-rc.5 DISTANCE 0 COMMIT 7d42b4e)
expect("${text}" "0.1.0-rc.5+0.g7d42b4e" "on the tag")

vss_compose_version(text VERSION 0.1.0-rc.5 DISTANCE 18 COMMIT 7d42b4e
  BUILD_NUMBER 6083)
expect("${text}" "0.1.0-rc.5+18.g7d42b4e.build.6083" "a CI build")

vss_compose_version(text VERSION 0.1.0-rc.5 DISTANCE 18 COMMIT 7d42b4e DIRTY)
expect("${text}" "0.1.0-rc.5+18.g7d42b4e.dirty" "a modified tree")

vss_compose_version(text VERSION 0.1.0-rc.5 DISTANCE 18 COMMIT 7d42b4e
  BUILD_NUMBER 6083 DIRTY)
expect("${text}" "0.1.0-rc.5+18.g7d42b4e.build.6083.dirty" "all of it")

vss_compose_version(text VERSION 0.1.0-dev DISTANCE "" COMMIT 7d42b4e)
expect("${text}" "0.1.0-dev+g7d42b4e" "no tag, so no distance")

vss_compose_version(text VERSION 0.1.0-dev DISTANCE 3 COMMIT unknown DIRTY)
expect("${text}" "0.1.0-dev" "no commit, so no distance and no dirty flag")

vss_compose_version(text VERSION 0.1.0-dev COMMIT "" BUILD_NUMBER 12)
expect("${text}" "0.1.0-dev+build.12" "a build number without a commit")

# A commit that looks like a number or a false constant is still a commit.
vss_compose_version(text VERSION 1.0.0 DISTANCE 1 COMMIT 0000000)
expect("${text}" "1.0.0+1.g0000000" "a commit of zeros")

# --- the stamp, from a real history -----------------------------------------

if(NOT GIT_EXECUTABLE OR NOT WORK_DIR)
  message(STATUS "version stamp: the composition passed; no git run")
  return()
endif()

# What a CI run sets must not reach the stamp made here.
foreach(name GITHUB_SHA GITHUB_REF_TYPE GITHUB_REF_NAME GITHUB_HEAD_REF
    GITHUB_RUN_NUMBER)
  unset(ENV{${name}})
endforeach()

set(repo "${WORK_DIR}/repo")
set(template "${WORK_DIR}/stamp.in")
set(output "${WORK_DIR}/stamp.txt")
execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -rf "${WORK_DIR}")
file(MAKE_DIRECTORY "${repo}")
file(WRITE "${template}"
  "@VSS_STAMP_full_version@|@VSS_STAMP_distance@|@VSS_STAMP_dirty@")

function(git)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}"
      -c user.name=test -c user.email=test@example.invalid
      -c commit.gpgsign=false -c tag.gpgsign=false
      ${ARGN}
    WORKING_DIRECTORY "${repo}"
    OUTPUT_VARIABLE text
    ERROR_VARIABLE problem
    RESULT_VARIABLE status
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "git ${ARGN} failed: ${problem}")
  endif()
  set(git_output "${text}" PARENT_SCOPE)
endfunction()

function(stamp out)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DIN=${template}" "-DOUT=${output}" "-DSOURCE_DIR=${repo}"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" "-DPROJECT_VERSION=0.1.0"
      "-DBUILD_TYPE=Debug" "-DCOMPILER=test"
      -P "${SOURCE_ROOT}/cmake/VssBuildStamp.cmake"
    RESULT_VARIABLE status
    ERROR_VARIABLE problem)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "the stamp script failed: ${problem}")
  endif()
  file(READ "${output}" text)
  string(STRIP "${text}" text)
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(short_head out)
  git(rev-parse HEAD)
  string(SUBSTRING "${git_output}" 0 7 short)
  set(${out} "${short}" PARENT_SCOPE)
endfunction()

git(init --quiet)
file(WRITE "${repo}/file.txt" "one\n")
git(add file.txt)
git(commit --quiet -m one)
short_head(first)

stamp(text)
expect("${text}" "0.1.0-dev+g${first}|0|false" "no tag in the history")

git(tag v0.2.0-rc.1)
stamp(text)
expect("${text}" "0.2.0-rc.1+0.g${first}|0|false" "on the tag")

file(WRITE "${repo}/file.txt" "two\n")
git(commit --quiet -a -m two)
file(WRITE "${repo}/file.txt" "three\n")
git(commit --quiet -a -m three)
short_head(third)
stamp(text)
expect("${text}" "0.2.0-rc.1+2.g${third}|2|false" "two commits after the tag")

file(WRITE "${repo}/file.txt" "four\n")
stamp(text)
expect("${text}" "0.2.0-rc.1+2.g${third}.dirty|2|true" "a modified tree")

# A tag that is not a version leaves the version alone and counts nothing.
git(checkout --quiet -- file.txt)
git(tag --force v-not-a-version)
git(tag --delete v0.2.0-rc.1)
stamp(text)
expect("${text}" "0.1.0-dev+g${third}|0|false" "a tag that is not a version")

# A version given to the build overrides git. It is not a tag of this
# repository, so there is nothing to count from and no distance is claimed.
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-DIN=${template}" "-DOUT=${output}" "-DSOURCE_DIR=${repo}"
    "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" "-DPROJECT_VERSION=0.1.0"
    "-DBUILD_TYPE=Debug" "-DCOMPILER=test" "-DVERSION_OVERRIDE=v9.9.9"
    -P "${SOURCE_ROOT}/cmake/VssBuildStamp.cmake"
  RESULT_VARIABLE status
  ERROR_VARIABLE problem)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "the stamp script failed: ${problem}")
endif()
file(READ "${output}" text)
string(STRIP "${text}" text)
expect("${text}" "9.9.9+g${third}|0|false" "a version given to the build")

execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -rf "${WORK_DIR}")
message(STATUS "version stamp: passed")
