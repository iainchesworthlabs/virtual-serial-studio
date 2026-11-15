# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes the copy of CPack's NSIS template that the Windows installer is built from.
#
# The template (Modules/Internal/CPack/NSIS.template.in in CMake) has one line the
# installer cannot do without changing: .onInit runs the uninstaller of the earlier
# version with
#
#   ExecWait '"$0" /S _?=$3'
#
# and the uninstaller has no way to tell that from `Uninstall.exe /S _?=<folder>`,
# which is also how an unattended uninstall is made to wait. The copy made here passes
# /VSSUPGRADE=1 as well, so that only an upgrade keeps the driver, the helper service
# and the PATH entry (driver.nsh reads the switch), and it remembers the folder of the
# earlier version in $VssUpgradeFolder for the clean-up when the upgrade is cancelled.
#
# Three things change, each in a place that must match exactly once:
#
#   1. the ExecWait line above (the switch, and the folder remembered before it);
#   2. a call at the label `inst:` that picks up an unfinished upgrade (a stash left
#      by an installer that was killed), see VssAdoptUnfinishedUpgrade in driver.nsh;
#   3. a !define before the line that carries CPACK_NSIS_DEFINES. driver.nsh stops the
#      build with !error when it is not defined, so an installer made from CPack's own
#      template (a CPACK_MODULE_PATH that points elsewhere, a build folder without the
#      copy) is an error, not an installer that passes no switch.
#
# Nothing else in the template is changed. When a CMake release moves one of the lines
# the replacement finds nothing and this stops the configure step; it does not build an
# installer that keeps nothing, or everything, by guess.

# Replaces `old` with `new` in the variable named by `variable` (in the caller's scope),
# and stops with an error unless `old` is there exactly once.
function(_vss_replace_once variable old new what)
  string(FIND "${${variable}}" "${old}" _vss_first)
  string(FIND "${${variable}}" "${old}" _vss_last REVERSE)
  if(_vss_first EQUAL -1)
    message(FATAL_ERROR
      "${what} is not in CPack's NSIS template:\n${old}\n"
      "cmake/packaging/windows/nsis-template.cmake has to be updated for this CMake.")
  endif()
  if(NOT _vss_first EQUAL _vss_last)
    message(FATAL_ERROR "${what} is in CPack's NSIS template more than once:\n${old}")
  endif()
  string(REPLACE "${old}" "${new}" _vss_changed "${${variable}}")
  set(${variable} "${_vss_changed}" PARENT_SCOPE)
endfunction()

# vss_write_nsis_template(<output folder>)
#
# Writes <output folder>/NSIS.template.in and sets VSS_NSIS_TEMPLATE_DIR in the
# caller's scope to <output folder>, which belongs in CPACK_MODULE_PATH.
function(vss_write_nsis_template output_dir)
  set(_vss_stock "${CMAKE_ROOT}/Modules/Internal/CPack/NSIS.template.in")
  if(NOT EXISTS "${_vss_stock}")
    message(FATAL_ERROR
      "CPack's NSIS template is not at ${_vss_stock}; "
      "cmake/packaging/windows/nsis-template.cmake needs to learn where this CMake keeps it.")
  endif()
  file(READ "${_vss_stock}" _vss_text)

  # 1. The line that runs the earlier version's uninstaller. NSIS wants _?= last on
  #    the command line, so the switch goes before it.
  _vss_replace_once(_vss_text
    "  ExecWait '\"$0\" /S _?=$3' ;Do not copy the uninstaller to a temp file"
    "  StrCpy $VssUpgradeFolder $3\n  ExecWait '\"$0\" /S /VSSUPGRADE=1 _?=$3' ;Do not copy the uninstaller to a temp file"
    "The line that runs the earlier version's uninstaller")

  # 2. Where every path of .onInit ends up, before the install folder is chosen.
  _vss_replace_once(_vss_text
    "\ninst:\n  ; Reads components status for registry\n"
    "\ninst:\n  Call VssAdoptUnfinishedUpgrade\n  ; Reads components status for registry\n"
    "The label that ends the uninstall of the earlier version in .onInit")

  # 3. The marker that driver.nsh requires.
  _vss_replace_once(_vss_text
    "\n@CPACK_NSIS_DEFINES@\n"
    "\n!define VSS_TEMPLATE_PASSES_UPGRADE_SWITCH\n@CPACK_NSIS_DEFINES@\n"
    "The line that holds CPACK_NSIS_DEFINES")

  file(MAKE_DIRECTORY "${output_dir}")
  file(WRITE "${output_dir}/NSIS.template.in" "${_vss_text}")
  set(VSS_NSIS_TEMPLATE_DIR "${output_dir}" PARENT_SCOPE)
endfunction()
