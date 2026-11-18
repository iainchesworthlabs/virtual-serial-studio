; Copyright (c) 2026 Virtual Serial Studio contributors
; SPDX-License-Identifier: GPL-3.0-or-later
;
; A throwaway installer that only asks the driver question of the real installer's driver page
; (VssDriverQuery in cmake/packaging/windows/driver.nsh) and writes the answer next to itself, in
; q.out, as [state][installed][bundled]. It installs nothing. tools/vm/driver-update-cell.ps1
; builds it with makensis and runs it in the guest, elevated, between the steps of the update
; scenario. The defines come from the command line:
;
;   makensis /DOUTFILE=<exe> /DDRIVER_NSH=<path of driver.nsh> /DSCRIPT=<path of setup-driver.ps1>
;            /DBUNDLED=<DriverVer, such as 11/18/2026,1.5.1.0> driver-query.nsi

Unicode true
!define VSS_REGISTRY_KEY 'q'
!define MUI_CUSTOMFUNCTION_ABORT VssOnUserAbort
!define VSS_BUNDLED_DRIVER_VER "${BUNDLED}"
!define VSS_DRIVER_QUERY_SCRIPT "${SCRIPT}"
!include MUI2.nsh
!define SHCTX HKLM
Name "driver-query"
OutFile "${OUTFILE}"
InstallDir "$TEMP\driver-query"
SilentInstall silent
RequestExecutionLevel admin
!define VSS_TEMPLATE_PASSES_UPGRADE_SWITCH
!include "${DRIVER_NSH}"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section
  Call VssDriverQuery
  FileOpen $0 "$EXEDIR\q.out" w
  FileWrite $0 "[$VssDriverState][$VssDriverInstalled][$VssDriverBundled]"
  FileClose $0
SectionEnd

Section Uninstall
  Call un.VssRemoveDriver
SectionEnd
