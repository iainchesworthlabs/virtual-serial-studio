; SPDX-License-Identifier: GPL-3.0-or-later
;
; The part of the Windows installer that sets up and takes away the virtual COM
; port driver and its helper service. CPack's NSIS template includes this file
; (CPACK_NSIS_DEFINES in cmake/packaging/VssPackage-windows.cmake) and calls
; VssInstallDriver at the end of the install and un.VssRemoveDriver at the start
; of the uninstall.
;
; The work is done by the scripts in <install folder>\bin\driver, the same ones
; Studio's "Set up Virtual Serial Studio" runs (drivers/windows/setup-driver.ps1
; and remove-driver.ps1). The installer only decides whether to run them, keeps
; their output in a log and tells the person what happened. It adds no
; certificate and does not touch the test-signing setting.
;
; The choice is a page with a box that is ticked, and the switch /DRIVER=0 on the
; command line (/DRIVER=1 is the default) for a silent install, which shows no
; page. VSS_REGISTRY_KEY is defined by the CMake file.

!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!insertmacro GetParameters
!insertmacro GetOptions

Var VssDriverWanted
Var VssDriverCheckbox
Var VssDriverLog

; The command line decides when there is no page: /DRIVER=0 skips the driver.
Function VssDriverSwitch
  Push $0
  Push $1
  ${GetParameters} $0
  ${GetOptions} $0 "/DRIVER=" $1
  ${If} $1 == "0"
    StrCpy $VssDriverWanted 0
  ${Else}
    StrCpy $VssDriverWanted 1
  ${EndIf}
  Pop $1
  Pop $0
FunctionEnd

Function VssDriverPageCreate
  Call VssDriverSwitch
  !insertmacro MUI_HEADER_TEXT "Virtual COM port driver" "Lets other programs open the serial ports that Studio makes."
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateCheckbox} 0 0 100% 12u "Install the virtual COM port driver and its helper service"
  Pop $VssDriverCheckbox
  ${If} $VssDriverWanted == 1
    ${NSD_Check} $VssDriverCheckbox
  ${EndIf}

  ${NSD_CreateLabel} 12u 18u 94% 52u "The driver gives Windows COM ports that other programs can open, with no com0com and no hardware. It is the user-mode driver (vsscom_um): it runs in a driver host process, not in the kernel, so test signing stays off. The helper service (vsscom_helper) runs as SYSTEM so that ordinary users can make ports. Both are removed when you uninstall Studio."
  Pop $0
  ${NSD_CreateLabel} 12u 76u 94% 52u "Windows must trust the signature on the driver files. If it does not, Studio is still installed, the driver is skipped, and you are told what to do. This installer adds no certificates and does not turn on test signing. To skip the driver in a silent install, run it with /S /DRIVER=0."
  Pop $0
  nsDialogs::Show
FunctionEnd

Function VssDriverPageLeave
  ${NSD_GetState} $VssDriverCheckbox $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $VssDriverWanted 1
  ${Else}
    StrCpy $VssDriverWanted 0
  ${EndIf}
FunctionEnd

; Adds a line to the log of the driver set-up.
Function VssLogLine
  Exch $R0
  Push $R1
  FileOpen $R1 $VssDriverLog a
  FileSeek $R1 0 END
  FileWrite $R1 "$R0$\r$\n"
  FileClose $R1
  Pop $R1
  Pop $R0
FunctionEnd

; Runs a script of the driver folder under the 64-bit PowerShell, with its output
; in the log. $0 is the exit code afterwards. This installer is a 32-bit program,
; so it reaches the 64-bit cmd through Sysnative; that cmd is 64-bit and finds
; PowerShell in System32 (Sysnative does not exist for it).
!macro VssRunScript script
  ExecWait '"$WINDIR\Sysnative\cmd.exe" /C ""$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\bin\driver\${script}" > "$VssDriverLog" 2>&1"' $0
!macroend

Function VssInstallDriver
  Push $0
  Push $1
  ${If} $VssDriverWanted == ""
    Call VssDriverSwitch
  ${EndIf}
  StrCpy $VssDriverLog "$INSTDIR\driver-setup.log"

  ${If} $VssDriverWanted != 1
    DetailPrint "Virtual COM port driver: not selected, so not installed."
    WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${VSS_REGISTRY_KEY}" "VssDriver" "skipped"
    Goto done
  ${EndIf}

  ${IfNot} ${FileExists} "$INSTDIR\bin\driver\setup-driver.ps1"
    DetailPrint "Virtual COM port driver: the installer does not hold the driver files."
    WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${VSS_REGISTRY_KEY}" "VssDriver" "failed-9"
    MessageBox MB_OK|MB_ICONEXCLAMATION "Virtual Serial Studio is installed, but this installer does not hold the driver files, so the virtual COM port driver was not installed." /SD IDOK
    Goto done
  ${EndIf}

  DetailPrint "Setting up the virtual COM port driver. The log is $VssDriverLog"
  !insertmacro VssRunScript "setup-driver.ps1"
  Push "setup-driver.ps1 exited with $0"
  Call VssLogLine

  ${If} $0 == 0
    DetailPrint "Virtual COM port driver: installed."
    WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${VSS_REGISTRY_KEY}" "VssDriver" "installed"
  ${ElseIf} $0 == 10
    DetailPrint "Virtual COM port driver: installed; Windows needs a restart before it is used."
    WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${VSS_REGISTRY_KEY}" "VssDriver" "installed-restart-needed"
    SetRebootFlag true
    MessageBox MB_OK|MB_ICONINFORMATION "The virtual COM port driver is installed. Windows needs a restart before it can be used." /SD IDOK
  ${ElseIf} $0 == 6
    DetailPrint "Virtual COM port driver: NOT installed, because Windows does not trust its signature."
    WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${VSS_REGISTRY_KEY}" "VssDriver" "not-trusted"
    MessageBox MB_OK|MB_ICONEXCLAMATION "Virtual Serial Studio is installed, but the virtual COM port driver is NOT.$\r$\n$\r$\nWindows does not trust the signature on the driver files. This installer does not change that: it adds no certificates and does not turn on test signing.$\r$\n$\r$\nTo install the driver later, on a machine you own:$\r$\n - run $INSTDIR\bin\driver\trust\install-trust.ps1 as administrator with the public certificates of the authority that signed this build (the script explains its arguments), or turn on Windows test signing on a test machine and restart;$\r$\n - then choose Set up Virtual Serial Studio in Studio.$\r$\n$\r$\nWhat happened is in $VssDriverLog" /SD IDOK
  ${Else}
    DetailPrint "Virtual COM port driver: NOT installed (code $0)."
    WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${VSS_REGISTRY_KEY}" "VssDriver" "failed-$0"
    MessageBox MB_OK|MB_ICONEXCLAMATION "Virtual Serial Studio is installed, but the virtual COM port driver could not be installed (code $0).$\r$\n$\r$\nYou can try again from Studio (Set up Virtual Serial Studio). What happened is in $VssDriverLog" /SD IDOK
  ${EndIf}

done:
  Pop $1
  Pop $0
FunctionEnd

; Takes the driver away when Studio is uninstalled. The installer of a newer
; version runs the old uninstaller in place (the uninstaller then runs from the
; install folder itself, which a normal uninstall never does) to replace the
; files, and that must not take the driver and its COM numbers away: the new
; installer sets it up again over the same package.
Function un.VssRemoveDriver
  Push $0
  ${If} $EXEDIR == $INSTDIR
    DetailPrint "Upgrade: the driver, the helper service and the ports stay in place."
    Goto done
  ${EndIf}

  ${IfNot} ${FileExists} "$INSTDIR\bin\driver\remove-driver.ps1"
    DetailPrint "No driver files to remove."
    Goto done
  ${EndIf}

  StrCpy $VssDriverLog "$INSTDIR\driver-remove.log"
  DetailPrint "Removing the virtual COM port driver and the helper service."
  !insertmacro VssRunScript "remove-driver.ps1"
  ${If} $0 == 0
  ${OrIf} $0 == 10
    Delete "$VssDriverLog"
    ${If} $0 == 10
      SetRebootFlag true
    ${EndIf}
  ${Else}
    MessageBox MB_OK|MB_ICONEXCLAMATION "Virtual Serial Studio is removed, but the virtual COM port driver or its helper service could not be removed completely (code $0).$\r$\n$\r$\nWhat happened is in $VssDriverLog" /SD IDOK
  ${EndIf}

done:
  Pop $0
FunctionEnd
