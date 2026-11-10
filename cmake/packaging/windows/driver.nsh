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
;
; The same file holds the PATH choice: a page with a box that is not ticked, and the
; switch /ADDTOPATH=1 for a silent install (nothing is added by default). CPack's own
; PATH code keeps the PATH in an NSIS string, which stops at 1024 characters and
; refuses a longer PATH, so the CMake file turns it off and bin\edit-path.ps1 does
; the work on the registry value itself.

!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "WinMessages.nsh"

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
; PowerShell in System32 (Sysnative does not exist for it). PSModulePath is cleared
; for the child: an installer started from PowerShell 7 (a build agent, a script)
; hands Windows PowerShell 5.1 the module path of PowerShell 7, and 5.1 then cannot
; load its own Microsoft.PowerShell.Security. Without the variable it uses its own.
!macro VssRunScript script
  ExecWait '"$WINDIR\Sysnative\cmd.exe" /C set "PSModulePath=" & "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\bin\driver\${script}" > "$VssDriverLog" 2>&1' $0
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

; ---- The PATH ----------------------------------------------------------------

Var VssPathWanted
Var VssPathCheckbox

; The command line decides when there is no page: /ADDTOPATH=1 adds the folder.
Function VssPathSwitch
  Push $0
  Push $1
  ${GetParameters} $0
  ${GetOptions} $0 "/ADDTOPATH=" $1
  ${If} $1 == "1"
    StrCpy $VssPathWanted 1
  ${Else}
    StrCpy $VssPathWanted 0
  ${EndIf}
  Pop $1
  Pop $0
FunctionEnd

Function VssPathPageCreate
  Call VssPathSwitch
  !insertmacro MUI_HEADER_TEXT "Command line" "Lets you run vss_cli from any terminal."
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateCheckbox} 0 0 100% 12u "Add the Virtual Serial Studio command line tool to the PATH for all users"
  Pop $VssPathCheckbox
  ${If} $VssPathWanted == 1
    ${NSD_Check} $VssPathCheckbox
  ${EndIf}

  ${NSD_CreateLabel} 12u 18u 94% 40u "This puts the folder of vss_cli.exe at the end of the system PATH, so that vss_cli can be run from any terminal opened afterwards. Nothing else in the PATH is changed. It is off unless you tick it, and the uninstaller takes the folder out again. To add it in a silent install, run the installer with /S /ADDTOPATH=1."
  Pop $0
  nsDialogs::Show
FunctionEnd

Function VssPathPageLeave
  ${NSD_GetState} $VssPathCheckbox $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $VssPathWanted 1
  ${Else}
    StrCpy $VssPathWanted 0
  ${EndIf}
FunctionEnd

; Runs bin\edit-path.ps1 with the action (Add or Remove); $0 is its exit code
; afterwards. Same 64-bit PowerShell route as the driver scripts, see VssRunScript.
; A macro, because the installer and the uninstaller each need their own copy.
!macro VssRunEditPath action
  ExecWait '"$WINDIR\Sysnative\cmd.exe" /C set "PSModulePath=" & "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\bin\edit-path.ps1" -Action ${action} -Directory "$INSTDIR\bin" > "$INSTDIR\path-setup.log" 2>&1' $0
!macroend

; Installs the script (it is always there, for the uninstaller) and, when chosen,
; adds the folder. A PATH that cannot be extended is reported, not half-changed.
Function VssInstallPath
  Push $0
  ${If} $VssPathWanted == ""
    Call VssPathSwitch
  ${EndIf}

  SetOutPath "$INSTDIR\bin"
  File "${__FILEDIR__}\edit-path.ps1"

  ; The stock remover in CPack's template reads this; the uninstaller below does the
  ; removal instead and the stock one must not touch the PATH.
  WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${VSS_REGISTRY_KEY}" "DoNotAddToPath" "1"

  ${If} $VssPathWanted != 1
    DetailPrint "PATH: not selected, so not changed."
    Goto done
  ${EndIf}

  DetailPrint "Adding $INSTDIR\bin to the PATH."
  !insertmacro VssRunEditPath "Add"
  ${If} $0 == 0
    DetailPrint "PATH: added."
    WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${VSS_REGISTRY_KEY}" "VssPath" "added"
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
  ${Else}
    DetailPrint "PATH: NOT changed (code $0)."
    WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${VSS_REGISTRY_KEY}" "VssPath" "failed-$0"
    MessageBox MB_OK|MB_ICONEXCLAMATION "Virtual Serial Studio is installed, but its folder was NOT added to the PATH (code $0). The PATH was not changed.$\r$\n$\r$\nTo run vss_cli from a terminal, use its full path, $INSTDIR\bin\vss_cli.exe, or add that folder to the PATH yourself.$\r$\n$\r$\nWhat happened is in $INSTDIR\path-setup.log" /SD IDOK
  ${EndIf}

done:
  Pop $0
FunctionEnd

; Takes the folder out of the PATH when Studio is uninstalled. An upgrade runs the old
; uninstaller in place (see un.VssRemoveDriver) and leaves the PATH as it is.
Function un.VssRemovePath
  Push $0
  ${If} $EXEDIR == $INSTDIR
    DetailPrint "Upgrade: the PATH stays as it is."
    Goto done
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\bin\edit-path.ps1"
    Goto done
  ${EndIf}

  DetailPrint "Taking $INSTDIR\bin out of the PATH."
  !insertmacro VssRunEditPath "Remove"
  ${If} $0 == 0
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
  ${Else}
    MessageBox MB_OK|MB_ICONEXCLAMATION "Virtual Serial Studio is removed, but its folder could not be taken out of the PATH (code $0). The PATH was not changed.$\r$\n$\r$\nWhat happened is in $INSTDIR\path-setup.log" /SD IDOK
  ${EndIf}

done:
  Pop $0
FunctionEnd
