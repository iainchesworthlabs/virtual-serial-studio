# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run by CPack (CPACK_PRE_BUILD_SCRIPTS) after the runtime component is staged
# and before the disk image is made. It signs and notarises the staged
# vss_gui.app and bin/vss_cli with tools/macos/sign-and-notarize.sh when the
# environment names a signing identity and a notary profile, and does nothing
# otherwise, so an image built without them is simply unsigned.
#
# Nothing secret is read here: the script takes VSS_SIGN_IDENTITY and
# VSS_NOTARY_PROFILE from the environment itself.

if("$ENV{VSS_SIGN_IDENTITY}" STREQUAL "" OR "$ENV{VSS_NOTARY_PROFILE}" STREQUAL "")
  message(STATUS "vss: VSS_SIGN_IDENTITY or VSS_NOTARY_PROFILE is not set; "
                 "the disk image is built unsigned")
else()
  file(GLOB_RECURSE _vss_apps LIST_DIRECTORIES true
    "${CPACK_TOPLEVEL_DIRECTORY}/*/vss_gui.app")
  file(GLOB_RECURSE _vss_clis LIST_DIRECTORIES false
    "${CPACK_TOPLEVEL_DIRECTORY}/*/vss_cli")
  list(LENGTH _vss_apps _vss_app_count)
  list(LENGTH _vss_clis _vss_cli_count)
  if(NOT _vss_app_count EQUAL 1 OR NOT _vss_cli_count EQUAL 1)
    message(FATAL_ERROR "vss: expected one vss_gui.app and one vss_cli under "
      "${CPACK_TOPLEVEL_DIRECTORY}, found ${_vss_app_count} and ${_vss_cli_count}")
  endif()
  execute_process(
    COMMAND bash "${CPACK_VSS_SIGN_SCRIPT}" --app "${_vss_apps}" --cli "${_vss_clis}"
    RESULT_VARIABLE _vss_sign_result)
  if(NOT _vss_sign_result EQUAL 0)
    message(FATAL_ERROR "vss: signing and notarising the staged app failed "
      "(${_vss_sign_result})")
  endif()
endif()
