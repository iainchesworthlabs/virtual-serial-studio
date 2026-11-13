# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# Writes the generated source file that tells the program which build it is.
# Run as `cmake -P` on every build (see VssBuildInfo.cmake), so the stamp
# follows new commits without a reconfigure.
#
# Required variables: IN (the .cpp.in template), OUT (the file to write),
# SOURCE_DIR, PROJECT_VERSION, BUILD_TYPE, COMPILER.
# Optional: GIT_EXECUTABLE, BUILD_NUMBER (overrides the CI run number),
# VERSION_OVERRIDE (a tag such as v1.2.3-rc.1, overrides git).
#
# Where each value comes from:
#   tag            the tag GitHub Actions built (GITHUB_REF_TYPE=tag), else the
#                  nearest v* tag git can see, else none (a shallow CI clone)
#   version        that tag without its leading v; 0.1.0-dev (the project()
#                  version) when there is no tag
#   build number   BUILD_NUMBER, else GITHUB_RUN_NUMBER, else none
#   commit         GITHUB_SHA, else git HEAD
#   branch         GITHUB_HEAD_REF (pull requests), else the branch GitHub
#                  Actions built, else the checked-out branch; on a detached
#                  HEAD the tag, or a branch that points at the commit
#   dirty          whether the working tree differs from the commit (git only)
#   build time     UTC, refreshed only when something above changed, so an
#                  unchanged tree does not relink on every build

function(vss_git out)
  set(${out} "" PARENT_SCOPE)
  if(NOT GIT_EXECUTABLE)
    return()
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" ${ARGN}
    OUTPUT_VARIABLE text
    RESULT_VARIABLE status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(status EQUAL 0)
    set(${out} "${text}" PARENT_SCOPE)
  endif()
endfunction()

function(vss_escape out text)
  string(REPLACE "\\" "\\\\" text "${text}")
  string(REPLACE "\"" "\\\"" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# --- commit -----------------------------------------------------------------
vss_git(head rev-parse HEAD)
set(commit_full "$ENV{GITHUB_SHA}")
if(NOT commit_full)
  set(commit_full "${head}")
endif()
if(commit_full)
  string(SUBSTRING "${commit_full}" 0 7 commit)
else()
  set(commit_full "unknown")
  set(commit "unknown")
endif()

# --- tag, version and distance from the tag ---------------------------------
set(tag "")
set(distance 0)
if(VERSION_OVERRIDE)
  set(tag "${VERSION_OVERRIDE}")
elseif("$ENV{GITHUB_REF_TYPE}" STREQUAL "tag")
  set(tag "$ENV{GITHUB_REF_NAME}")
else()
  vss_git(tag describe --tags --match "v*" --abbrev=0 HEAD)
  if(tag)
    vss_git(distance rev-list --count "${tag}..HEAD")
  endif()
endif()
if(NOT distance)
  set(distance 0)
endif()

set(version "${PROJECT_VERSION}-dev")
if(tag MATCHES "^v?([0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?)$")
  set(version "${CMAKE_MATCH_1}")
endif()
if(NOT tag)
  set(tag_text "")
else()
  set(tag_text "${tag}")
endif()

# --- branch -----------------------------------------------------------------
set(branch "$ENV{GITHUB_HEAD_REF}")
if(NOT branch AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "branch")
  set(branch "$ENV{GITHUB_REF_NAME}")
endif()
if(NOT branch)
  vss_git(branch branch --show-current)
endif()
if(NOT branch)
  # Detached HEAD: a tag build is on the tag; otherwise ask for a branch that
  # points at this commit.
  if("$ENV{GITHUB_REF_TYPE}" STREQUAL "tag")
    set(branch "$ENV{GITHUB_REF_NAME}")
  else()
    vss_git(exact describe --tags --exact-match HEAD)
    if(exact)
      set(branch "${exact}")
    else()
      vss_git(refs for-each-ref --points-at HEAD
        "--format=%(refname:short)" refs/heads refs/remotes)
      string(REPLACE "\n" ";" refs "${refs}")
      foreach(ref IN LISTS refs)
        if(NOT ref MATCHES "(^|/)HEAD$")
          set(branch "${ref}")
          break()
        endif()
      endforeach()
    endif()
  endif()
endif()
if(NOT branch)
  set(branch "unknown")
endif()

# --- dirty ------------------------------------------------------------------
set(dirty "false")
if(GIT_EXECUTABLE AND head)
  vss_git(changes status --porcelain)
  if(changes)
    set(dirty "true")
  endif()
endif()

# --- build number -----------------------------------------------------------
set(build_number "${BUILD_NUMBER}")
if(NOT build_number)
  set(build_number "$ENV{GITHUB_RUN_NUMBER}")
endif()

# --- write ------------------------------------------------------------------
set(state "${version}|${tag_text}|${distance}|${commit_full}|${branch}|${dirty}|${build_number}|${BUILD_TYPE}|${COMPILER}")
set(state_file "${OUT}.state")
set(previous "")
if(EXISTS "${state_file}" AND EXISTS "${OUT}")
  file(READ "${state_file}" previous)
endif()
if(previous STREQUAL state)
  return()
endif()

string(TIMESTAMP build_time "%Y-%m-%dT%H:%M:%SZ" UTC)
foreach(name version tag_text commit commit_full branch build_number build_time)
  vss_escape(VSS_STAMP_${name} "${${name}}")
endforeach()
vss_escape(VSS_STAMP_build_type "${BUILD_TYPE}")
vss_escape(VSS_STAMP_compiler "${COMPILER}")
set(VSS_STAMP_distance "${distance}")
set(VSS_STAMP_dirty "${dirty}")
configure_file("${IN}" "${OUT}" @ONLY)
file(WRITE "${state_file}" "${state}")
