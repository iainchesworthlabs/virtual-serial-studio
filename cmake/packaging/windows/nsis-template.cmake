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
# Nothing else in the template is changed. When a CMake release moves that line the
# replacement finds nothing and this stops the configure step; it does not build an
# installer that keeps nothing, or everything, by guess.

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

  set(_vss_old "  ExecWait '\"$0\" /S _?=$3' ;Do not copy the uninstaller to a temp file")
  string(FIND "${_vss_text}" "${_vss_old}" _vss_at)
  if(_vss_at EQUAL -1)
    message(FATAL_ERROR
      "The line that runs the earlier version's uninstaller is not in ${_vss_stock}:\n"
      "${_vss_old}\n"
      "cmake/packaging/windows/nsis-template.cmake has to be updated for this CMake.")
  endif()
  string(FIND "${_vss_text}" "${_vss_old}" _vss_last REVERSE)
  if(NOT _vss_at EQUAL _vss_last)
    message(FATAL_ERROR
      "The line that runs the earlier version's uninstaller is in ${_vss_stock} more than once.")
  endif()

  # NSIS wants _?= last on the command line, so the switch goes before it.
  set(_vss_new
    "  StrCpy $VssUpgradeFolder $3\n  ExecWait '\"$0\" /S /VSSUPGRADE=1 _?=$3' ;Do not copy the uninstaller to a temp file")
  string(REPLACE "${_vss_old}" "${_vss_new}" _vss_text "${_vss_text}")

  file(MAKE_DIRECTORY "${output_dir}")
  file(WRITE "${output_dir}/NSIS.template.in" "${_vss_text}")
  set(VSS_NSIS_TEMPLATE_DIR "${output_dir}" PARENT_SCOPE)
endfunction()
