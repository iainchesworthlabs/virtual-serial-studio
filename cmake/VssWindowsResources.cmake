# SPDX-License-Identifier: GPL-3.0-or-later
#
# Windows resources for every program the project builds: an application
# manifest and a version resource.
#
# The manifest states requestedExecutionLevel asInvoker, long path and UTF-8
# awareness and the Windows versions the program was written for.  Without a
# manifest Windows guesses whether a program needs elevation, from its file name
# and version strings, and a wrong guess shows a UAC prompt the program never
# asked for.  The version resource gives Explorer, Task Manager, the firewall
# prompt and the UAC prompt a name to show.
#
# Nothing here is called by hand.  Including this file defers one call to the end
# of the top-level directory, which visits every executable the project defines
# (not those of fetched dependencies) and attaches the resources.  A target can
# set its own text before that point:
#
#   set_property(TARGET vss_cli PROPERTY VSS_FILE_DESCRIPTION "...")
#
# A target with a VSS_ICON property (the path of an .ico file) also gets that
# icon as its first icon resource:
#
#   set_property(TARGET vss_gui PROPERTY VSS_ICON "<dir>/vss_gui.ico")
#
# Do not put the words setup, install, update or patch in a description: they
# are the words the Windows installer heuristic looks for.
#
# On other platforms the file does nothing.

include_guard(GLOBAL)

if(NOT WIN32)
  return()
endif()

enable_language(RC)

set(_vss_windows_resources_dir "${CMAKE_CURRENT_LIST_DIR}/windows")

# The text Windows shows for a program that did not ask for its own.
function(_vss_default_file_description target out)
  if(target MATCHES "_(unit_|system_|header_)?(tests|selftest)$"
     OR target MATCHES "^vss_.*_tests$")
    set(_text "Virtual Serial Studio test program (${target})")
  elseif(target STREQUAL "vss_gui")
    set(_text "Virtual Serial Studio")
  elseif(target STREQUAL "vss_cli")
    set(_text "Virtual Serial Studio command line")
  elseif(target STREQUAL "vss_perf")
    set(_text "Virtual Serial Studio performance suite")
  else()
    set(_text "Virtual Serial Studio program (${target})")
  endif()
  set(${out} "${_text}" PARENT_SCOPE)
endfunction()

# vss_windows_resources(<executable target>)
function(vss_windows_resources target)
  get_target_property(_type ${target} TYPE)
  if(NOT _type STREQUAL "EXECUTABLE")
    return()
  endif()

  get_target_property(_output_name ${target} OUTPUT_NAME)
  if(NOT _output_name)
    set(_output_name ${target})
  endif()

  get_target_property(_description ${target} VSS_FILE_DESCRIPTION)
  if(NOT _description)
    _vss_default_file_description(${target} _description)
  endif()

  get_target_property(_icon ${target} VSS_ICON)
  if(_icon)
    # CMake paths use forward slashes, which are safe inside the quotes of an .rc.
    set(VSS_ICON_RESOURCE "1 ICON \"${_icon}\"")
  else()
    set(VSS_ICON_RESOURCE "")
  endif()

  set(VSS_TARGET "${target}")
  set(VSS_OUTPUT_NAME "${_output_name}")
  set(VSS_FILE_DESCRIPTION "${_description}")
  set(VSS_MANIFEST_NAME "IainChesworthLabs.VirtualSerialStudio.${target}")
  set(VSS_VERSION_DOTTED
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")
  set(VSS_MANIFEST_VERSION "${VSS_VERSION_DOTTED}")
  set(VSS_VERSION_COMMA
    "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")

  set(_dir "${CMAKE_BINARY_DIR}/windows-resources/${target}")
  set(VSS_MANIFEST_FILE "${_dir}/${target}.manifest")
  configure_file("${_vss_windows_resources_dir}/vss.manifest"
    "${VSS_MANIFEST_FILE}" @ONLY)

  # The MSVC linker (and lld-link) merge a .manifest source with the manifest
  # they generate, which carries their own settings.  A compiler without that
  # support gets the manifest as a resource of the .rc instead.
  if(MSVC)
    set(VSS_MANIFEST_RESOURCE "")
    target_sources(${target} PRIVATE "${VSS_MANIFEST_FILE}")
  else()
    # CMake paths use forward slashes, which are safe inside the quotes of an .rc.
    set(VSS_MANIFEST_RESOURCE "1 24 \"${VSS_MANIFEST_FILE}\"")
  endif()

  configure_file("${_vss_windows_resources_dir}/vss.rc.in"
    "${_dir}/${target}.rc" @ONLY)
  target_sources(${target} PRIVATE "${_dir}/${target}.rc")
  if(_icon)
    # Rebuild the resource when the icon file changes.
    set_source_files_properties("${_dir}/${target}.rc"
      TARGET_DIRECTORY ${target}
      PROPERTIES OBJECT_DEPENDS "${_icon}")
  endif()
endfunction()

function(_vss_collect_directory_targets directory out)
  get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(_subdirectory IN LISTS _subdirectories)
    _vss_collect_directory_targets("${_subdirectory}" _more)
    list(APPEND _targets ${_more})
  endforeach()
  set(${out} "${_targets}" PARENT_SCOPE)
endfunction()

function(_vss_windows_resources_everywhere source_dir binary_dir)
  _vss_collect_directory_targets("${source_dir}" _all)
  foreach(_target IN LISTS _all)
    get_target_property(_target_source_dir ${_target} SOURCE_DIR)
    # Fetched dependencies build inside the binary directory, and a library
    # built from a checkout outside the tree is not ours either.
    cmake_path(IS_PREFIX binary_dir "${_target_source_dir}" NORMALIZE _in_build)
    cmake_path(IS_PREFIX source_dir "${_target_source_dir}" NORMALIZE _in_tree)
    if(_in_tree AND NOT _in_build)
      vss_windows_resources(${_target})
    endif()
  endforeach()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL _vss_windows_resources_everywhere
  "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
