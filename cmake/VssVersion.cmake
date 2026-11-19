# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# vss_compose_version(<out> VERSION <version> [DISTANCE <n>] [COMMIT <sha>]
#                     [BUILD_NUMBER <n>] [DIRTY])
#
# Writes the one string that says which build this is, in the form Semantic
# Versioning gives to build metadata after a plus sign:
#
#   0.1.0-rc.5                              the release, nothing else known
#   0.1.0-rc.5+18.g7d42b4e                  18 commits after the tag, commit
#   0.1.0-rc.5+18.g7d42b4e.build.6083       a CI build with its run number
#   0.1.0-rc.5+18.g7d42b4e.dirty            the tree differed from the commit
#   0.1.0-dev+g7d42b4e                      no tag was visible, so no distance
#
# It is what `git describe --long --dirty` says, written so that it stays a
# valid Semantic Version: the part after the plus sign takes no part in the
# order, so 0.1.0-rc.5+18.g7d42b4e is still "0.1.0-rc.5" to anything that
# compares versions. Plugin manifests carry the same form (docs/plugins.md).
#
# DISTANCE is the number of commits after the tag that VERSION comes from; leave
# it out, or empty, when that is not known. 0 is a number: the build is on the
# tag. The commit and DIRTY are only written when there is a commit, and the
# build number whenever it is given.

function(vss_compose_version out)
  cmake_parse_arguments(ARG "DIRTY" "VERSION;DISTANCE;COMMIT;BUILD_NUMBER" ""
    ${ARGN})
  set(known_commit FALSE)
  if(NOT "${ARG_COMMIT}" STREQUAL "" AND NOT "${ARG_COMMIT}" STREQUAL "unknown")
    set(known_commit TRUE)
  endif()

  set(words "")
  if(known_commit)
    if(NOT "${ARG_DISTANCE}" STREQUAL "")
      list(APPEND words "${ARG_DISTANCE}")
    endif()
    list(APPEND words "g${ARG_COMMIT}")
  endif()
  if(NOT "${ARG_BUILD_NUMBER}" STREQUAL "")
    list(APPEND words "build" "${ARG_BUILD_NUMBER}")
  endif()
  if(known_commit AND ARG_DIRTY)
    list(APPEND words "dirty")
  endif()

  set(version "${ARG_VERSION}")
  if(words)
    list(JOIN words "." metadata)
    string(APPEND version "+${metadata}")
  endif()
  set(${out} "${version}" PARENT_SCOPE)
endfunction()
