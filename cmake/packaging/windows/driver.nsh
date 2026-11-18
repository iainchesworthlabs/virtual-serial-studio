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
; The driver package has a version (DriverVer in its .inf: a date, then four numbers). The page
; asks setup-driver.ps1 -Query what the driver store holds, and says so:
;   none      the box installs the driver and the helper service (ticked);
;   older     the box updates the driver to the newer one in this installer (ticked);
;   same      the driver is left as it is; the box "Install it again (repair)" is not ticked;
;   newer     the driver is left alone and the page says so; there is no box.
; In every case where the setup runs, the helper service is brought up to date: it is a program
; of this version of Studio. A silent install (/S) updates the driver only when this installer's
; is newer or there is none, and /REPAIRDRIVER=1 installs it again. setup-driver.ps1 makes the
; same decision itself, so a run from Studio or by hand never replaces a newer driver with an
; older one. The CMake file defines VSS_BUNDLED_DRIVER_VER (the DriverVer of vsscom_um.inf) and
; VSS_DRIVER_QUERY_SCRIPT (the path of setup-driver.ps1); either may be empty, and then the
; installer does not know and leaves the decision to the script.
!ifndef VSS_BUNDLED_DRIVER_VER
  !define VSS_BUNDLED_DRIVER_VER ""
!endif
!ifndef VSS_DRIVER_QUERY_SCRIPT
  !define VSS_DRIVER_QUERY_SCRIPT ""
!endif
;
; The same file holds the PATH choice: a page with a box that is not ticked, and the
; switch /ADDTOPATH=1 for a silent install (nothing is added by default). CPack's own
; PATH code keeps the PATH in an NSIS string, which stops at 1024 characters and
; refuses a longer PATH, so the CMake file turns it off and bin\edit-path.ps1 does
; the work on the registry value itself.

; The installer must be built from the copy of CPack's template that
; cmake/packaging/windows/nsis-template.cmake writes, which defines this. From CPack's
; own template the old uninstaller would be started without the upgrade switch, and
; every upgrade would remove the driver, the helper service and the PATH entry.
!ifndef VSS_TEMPLATE_PASSES_UPGRADE_SWITCH
  !error "This installer was not built from the template copy that passes /VSSUPGRADE=1 (cmake/packaging/windows/nsis-template.cmake). Check CPACK_MODULE_PATH."
!endif

!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "WinMessages.nsh"

!include "TextFunc.nsh"

!insertmacro GetParameters
!insertmacro GetOptions
!insertmacro TrimNewLines
!insertmacro un.GetParameters
!insertmacro un.GetOptions

Var VssDriverWanted
Var VssDriverCheckbox
Var VssDriverLog
; What the driver store holds compared with this installer's driver: none, older, same, newer or
; unknown (empty until VssDriverQuery has run), the two versions as text, and whether the driver is
; to be installed again whatever its version.
Var VssDriverState
Var VssDriverInstalled
Var VssDriverBundled
Var VssDriverRepair
Var VssDriverArgs

; An upgrade is told to the old uninstaller, never guessed. The installer of a newer
; version runs the old uninstaller with /VSSUPGRADE=1 (cmake/packaging/windows/
; nsis-template.cmake changes that one line of CPack's template). Only an uninstaller
; started with that switch keeps the driver, the helper service, the PATH entry and the
; per-user entries; any other uninstall removes them, whatever folder it runs from
; (Uninstall.exe /S _?=<folder> is how an unattended uninstall is made to wait, and it
; runs from the install folder too; it removes them, and leaves Uninstall.exe and the
; folder, which an uninstaller started that way cannot delete). The switch is for the
; installer's own use. Passed by hand to an uninstall it keeps the driver, the helper
; service and the PATH entry, removes Studio, and leaves the scripts that remove them
; in undo-upgrade.
;
; VssIsUpgrade is 1 in an uninstaller started with the switch, and empty otherwise.
; VssUpgradeFolder is, in the installer, the folder of the version whose uninstaller it
; ran, and empty when it ran none. It is what the clean-up of a cancelled upgrade uses.
Var VssIsUpgrade
Var VssUpgradeFolder

; The folder of the upgrade stash: what the old uninstaller leaves for the new
; installer to take the driver, the helper service and the PATH entry away again if
; the upgrade does not finish. It is inside the install folder, which only
; administrators can write when it is under Program Files (the default). A folder
; chosen outside it has the access list of its parent, as the other scripts in the
; install folder do.
!define VSS_UNDO_DIR "undo-upgrade"

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
  ${GetOptions} $0 "/REPAIRDRIVER=" $1
  ${If} $1 == "1"
    StrCpy $VssDriverRepair 1
  ${Else}
    StrCpy $VssDriverRepair 0
  ${EndIf}
  Pop $1
  Pop $0
FunctionEnd

; Asks setup-driver.ps1 -Query what the driver store holds, once. The script is taken out of the
; installer into its own temporary folder, because the files are not in the install folder yet when
; the page is shown. It changes nothing and needs no prompt, so it cannot wait for anyone; the
; answer is three lines in a file. Anything that goes wrong leaves the state "unknown".
Function VssDriverQuery
  Push $0
  Push $1
  Push $2
  ${If} $VssDriverState != ""
    Goto done
  ${EndIf}
  StrCpy $VssDriverState "unknown"
  StrCpy $VssDriverInstalled ""
  StrCpy $VssDriverBundled ""
  !if "${VSS_DRIVER_QUERY_SCRIPT}" != ""
  ${If} "${VSS_BUNDLED_DRIVER_VER}" != ""
    InitPluginsDir
    File "/oname=$PLUGINSDIR\setup-driver.ps1" "${VSS_DRIVER_QUERY_SCRIPT}"
    Delete "$PLUGINSDIR\driver-query.txt"
    ExecWait '"$WINDIR\Sysnative\cmd.exe" /C set "PSModulePath=" & "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\setup-driver.ps1" -Query -BundledVer "${VSS_BUNDLED_DRIVER_VER}" -QueryOut "$PLUGINSDIR\driver-query.txt" > "$PLUGINSDIR\driver-query.log" 2>&1' $0
    ${If} $0 == 0
    ${AndIf} ${FileExists} "$PLUGINSDIR\driver-query.txt"
      FileOpen $1 "$PLUGINSDIR\driver-query.txt" r
      FileRead $1 $2
      ${TrimNewLines} $2 $VssDriverState
      FileRead $1 $2
      ${TrimNewLines} $2 $VssDriverInstalled
      FileRead $1 $2
      ${TrimNewLines} $2 $VssDriverBundled
      FileClose $1
      ${If} $VssDriverState == ""
        StrCpy $VssDriverState "unknown"
      ${EndIf}
    ${EndIf}
  ${EndIf}
  !endif

done:
  Pop $2
  Pop $1
  Pop $0
FunctionEnd

Function VssDriverPageCreate
  Call VssDriverSwitch
  Call VssDriverQuery
  !insertmacro MUI_HEADER_TEXT "Virtual COM port driver" "Lets other programs open the serial ports that Studio makes."
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}
  StrCpy $VssDriverCheckbox ""

  ${If} $VssDriverState == "same"
    ${NSD_CreateLabel} 0 0 100% 36u "A Virtual Serial Studio driver is already installed: version $VssDriverInstalled, the same as the one in this installer. It is left as it is, so ports that are open stay open. The helper service is brought up to date."
    Pop $0
    ${NSD_CreateCheckbox} 0 40u 100% 12u "Install the driver again (repair)"
    Pop $VssDriverCheckbox
    ${If} $VssDriverRepair == 1
      ${NSD_Check} $VssDriverCheckbox
    ${EndIf}
    ${NSD_CreateLabel} 12u 56u 94% 30u "Only if the driver does not work. Installing it again closes the ports that are open now."
    Pop $0
  ${ElseIf} $VssDriverState == "newer"
    ${NSD_CreateLabel} 0 0 100% 44u "A Virtual Serial Studio driver is already installed: version $VssDriverInstalled. It is newer than the one in this installer (version $VssDriverBundled), so it is left alone. The helper service is brought up to date."
    Pop $0
  ${ElseIf} $VssDriverState == "older"
    ${NSD_CreateLabel} 0 0 100% 28u "A Virtual Serial Studio driver is already installed: version $VssDriverInstalled. This installer has the newer version $VssDriverBundled."
    Pop $0
    ${NSD_CreateCheckbox} 0 32u 100% 12u "Update the virtual COM port driver and its helper service"
    Pop $VssDriverCheckbox
    ${If} $VssDriverWanted == 1
      ${NSD_Check} $VssDriverCheckbox
    ${EndIf}
    ${NSD_CreateLabel} 12u 48u 94% 40u "Updating restarts the helper service, so the ports that are open now are closed and a program that has one open loses it. Studio offers to make the ports again. Windows may ask for a restart before the new driver is used."
    Pop $0
  ${Else}
    ${NSD_CreateCheckbox} 0 0 100% 12u "Install the virtual COM port driver and its helper service"
    Pop $VssDriverCheckbox
    ${If} $VssDriverWanted == 1
      ${NSD_Check} $VssDriverCheckbox
    ${EndIf}

    ${NSD_CreateLabel} 12u 18u 94% 52u "The driver gives Windows COM ports that other programs can open, with no com0com and no hardware. It is the user-mode driver (vsscom_um): it runs in a driver host process, not in the kernel, so test signing stays off. The helper service (vsscom_helper) runs as SYSTEM so that ordinary users can make ports. Both are removed when you uninstall Studio."
    Pop $0
  ${EndIf}
  ${NSD_CreateLabel} 12u 92u 94% 40u "Windows must trust the signature on the driver files. If it does not, Studio is still installed, the driver is skipped, and you are told what to do. This installer adds no certificates and does not turn on test signing. For a silent install, /S updates the driver only when this installer's is newer, /DRIVER=0 leaves the driver and the helper service alone, and /REPAIRDRIVER=1 installs the driver again."
  Pop $0
  nsDialogs::Show
FunctionEnd

Function VssDriverPageLeave
  ${If} $VssDriverCheckbox == ""
    Return
  ${EndIf}
  ${NSD_GetState} $VssDriverCheckbox $0
  ${If} $VssDriverState == "same"
    ; The box is the repair: the driver step runs either way, and installs the driver again when ticked.
    ${If} $0 == ${BST_CHECKED}
      StrCpy $VssDriverRepair 1
    ${Else}
      StrCpy $VssDriverRepair 0
    ${EndIf}
  ${ElseIf} $0 == ${BST_CHECKED}
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

; The same with the extra arguments of the script in $VssDriverArgs, each starting with a space.
!macro VssRunScriptArgs script
  ExecWait '"$WINDIR\Sysnative\cmd.exe" /C set "PSModulePath=" & "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\bin\driver\${script}"$VssDriverArgs > "$VssDriverLog" 2>&1' $0
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

  ; What is there, said before the script runs. The script decides the same way from the same
  ; numbers: a driver of the same or a newer version is left as it is unless it is to be installed again.
  Call VssDriverQuery
  ${If} $VssDriverState == "none"
    DetailPrint "Virtual COM port driver: no Studio driver is installed yet."
  ${ElseIf} $VssDriverState == "older"
    DetailPrint "Virtual COM port driver: version $VssDriverInstalled is installed; updating it to version $VssDriverBundled."
  ${ElseIf} $VssDriverState == "same"
    ${If} $VssDriverRepair == 1
      DetailPrint "Virtual COM port driver: version $VssDriverInstalled is installed, the same as this installer; installing it again."
    ${Else}
      DetailPrint "Virtual COM port driver: version $VssDriverInstalled is installed, the same as this installer; leaving it as it is."
    ${EndIf}
  ${ElseIf} $VssDriverState == "newer"
    ${If} $VssDriverRepair == 1
      DetailPrint "Virtual COM port driver: version $VssDriverInstalled is installed, newer than this installer's $VssDriverBundled; installing this installer's again."
    ${Else}
      DetailPrint "Virtual COM port driver: version $VssDriverInstalled is installed, newer than this installer's $VssDriverBundled; leaving it alone."
    ${EndIf}
  ${Else}
    DetailPrint "Virtual COM port driver: the installed version could not be read; the setup script decides."
  ${EndIf}

  DetailPrint "Setting up the virtual COM port driver. The log is $VssDriverLog"
  StrCpy $VssDriverArgs ""
  ${If} $VssDriverRepair == 1
    StrCpy $VssDriverArgs " -ReinstallDriver"
  ${EndIf}
  !insertmacro VssRunScriptArgs "setup-driver.ps1"
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
; version runs the old uninstaller with /VSSUPGRADE=1 to replace the files, and that
; must not take the driver and its COM numbers away: the new installer sets it up
; again over the same package, and takes it away itself if the upgrade is cancelled.
; An uninstaller started without the switch always removes it.
Function un.VssRemoveDriver
  Push $0
  ${If} $VssIsUpgrade == 1
    DetailPrint "Upgrade: the driver, the helper service and the ports stay in place."
    Goto done
  ${EndIf}

  ${IfNot} ${FileExists} "$INSTDIR\bin\driver\remove-driver.ps1"
    DetailPrint "No driver files to remove."
    Goto done
  ${EndIf}

  StrCpy $VssDriverLog "$INSTDIR\driver-remove.log"
  DetailPrint "Removing the helper service, the COM ports Studio made, and every Virtual Serial Studio driver package in the driver store, the older versions that updates left behind included. The log says which."
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

; Takes the folder out of the PATH when Studio is uninstalled. An upgrade (an
; uninstaller started with /VSSUPGRADE=1, see un.VssRemoveDriver) leaves the PATH as it
; is. path-setup.log is the log of the PATH work. It is deleted with the rest, except
; when taking the folder out failed: then it stays in the install folder, which stays
; with it, and the message says so, because it is the only thing left that says why the
; PATH still names a folder that is gone.
Function un.VssRemovePath
  Push $0
  ${If} $VssIsUpgrade == 1
    DetailPrint "Upgrade: the PATH stays as it is."
    Goto clean
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\bin\edit-path.ps1"
    Goto clean
  ${EndIf}

  DetailPrint "Taking $INSTDIR\bin out of the PATH."
  !insertmacro VssRunEditPath "Remove"
  ${If} $0 == 0
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
  ${Else}
    DetailPrint "PATH: NOT changed (code $0). The log is $INSTDIR\path-setup.log"
    MessageBox MB_OK|MB_ICONEXCLAMATION "Virtual Serial Studio is removed, but its folder could not be taken out of the PATH (code $0). The PATH was not changed.$\r$\n$\r$\nThe folder $INSTDIR\bin is still on it. What happened is in $INSTDIR\path-setup.log, which stays there for you to read." /SD IDOK
    Goto done
  ${EndIf}

clean:
  Delete "$INSTDIR\path-setup.log"

done:
  ; The script is not in CPack's list of installed files (VssInstallPath puts it
  ; there), so the uninstaller would leave it, and the folder with it.
  Delete "$INSTDIR\bin\edit-path.ps1"
  Pop $0
FunctionEnd

; ---- Running programs, and what a removed Studio leaves with each user ---------
;
; Windows cannot replace or delete a program that runs, so an upgrade over a running
; Studio kept the old vss_gui.exe and an uninstall left the folder behind. Both now
; close vss_gui.exe and vss_cli.exe first, but only the ones that run from this
; install folder (bin\stop-studio.ps1; a Studio from the zip is not touched). Studio
; keeps what runs in the user's running-setup.json, so the ports come back the next
; time it starts.
;
; A real uninstall also takes away each user's "Start when I sign in" entry and
; records in each running-setup.json that the ports were stopped, so that a Studio
; installed again offers them and does not start them by itself. An upgrade leaves
; both, as it leaves the driver and the PATH.

; Runs stop-studio.ps1 with an action; $0 is its exit code afterwards. Same 64-bit
; PowerShell route as the driver scripts, see VssRunScript. A macro, because the
; installer and the uninstaller each need their own copy.
!macro VssRunStopStudio script action log
  ExecWait '"$WINDIR\Sysnative\cmd.exe" /C set "PSModulePath=" & "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "${script}" -Action ${action} -Directory "$INSTDIR\bin" > "${log}" 2>&1' $0
!macroend

; Before the files are copied. An uninstaller that knows how has closed Studio
; already (the installer ran it before this); this is for a folder that holds
; programs without one, and it asks nobody, because the old version is gone by now
; and stopping here would leave half an install.
Function VssStopStudio
  Push $0
  ${IfNot} ${FileExists} "$INSTDIR\bin\*.exe"
    Goto done
  ${EndIf}
  InitPluginsDir
  File "/oname=$PLUGINSDIR\stop-studio.ps1" "${__FILEDIR__}\stop-studio.ps1"
  !insertmacro VssRunStopStudio "$PLUGINSDIR\stop-studio.ps1" "Stop" "$PLUGINSDIR\stop-studio.log"
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONSTOP "Virtual Serial Studio is running from $INSTDIR\bin and could not be closed (code $0), so its files cannot be replaced.$\r$\n$\r$\nClose it (Stop the ports and quit, in its notification-area menu), then run this installer again." /SD IDOK
    Abort "Virtual Serial Studio is still running, so the files were not replaced."
  ${EndIf}

done:
  Pop $0
FunctionEnd

; Puts the script in the install folder, for the uninstaller.
Function VssInstallStopStudio
  SetOutPath "$INSTDIR\bin"
  File "${__FILEDIR__}\stop-studio.ps1"
FunctionEnd

; First in the uninstall, before anything is taken away. A person who uninstalls is
; asked before a running Studio is closed; an upgrade and a silent uninstall ask
; nobody.
Function un.VssStopStudio
  Push $0
  ${IfNot} ${FileExists} "$INSTDIR\bin\stop-studio.ps1"
    Goto done
  ${EndIf}

  !insertmacro VssRunStopStudio "$INSTDIR\bin\stop-studio.ps1" "Check" "$INSTDIR\stop-studio.log"
  ${If} $0 != 1
    Delete "$INSTDIR\stop-studio.log"
    Goto done
  ${EndIf}

  ${If} $VssIsUpgrade != 1
    MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION "Virtual Serial Studio is running. Uninstalling closes it, and that stops its ports." /SD IDOK IDOK close
    Delete "$INSTDIR\stop-studio.log"
    Abort "Virtual Serial Studio is still running, so nothing was removed."
  ${EndIf}

close:
  DetailPrint "Closing Virtual Serial Studio."
  !insertmacro VssRunStopStudio "$INSTDIR\bin\stop-studio.ps1" "Stop" "$INSTDIR\stop-studio.log"
  ${If} $0 == 0
    Delete "$INSTDIR\stop-studio.log"
  ${Else}
    MessageBox MB_OK|MB_ICONEXCLAMATION "Virtual Serial Studio is still running and could not be closed (code $0). The files it uses stay until it is closed.$\r$\n$\r$\nWhat happened is in $INSTDIR\stop-studio.log" /SD IDOK
  ${EndIf}

done:
  Pop $0
FunctionEnd

; A real uninstall only: the sign-in entry of each user, and the note in each
; running setup that the ports were stopped. See stop-studio.ps1 for what it will
; not do in a folder that belongs to somebody else.
Function un.VssForgetUsers
  Push $0
  ${If} $VssIsUpgrade == 1
    DetailPrint "Upgrade: start at sign-in and each user's running setup stay as they are."
    Goto done
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\bin\stop-studio.ps1"
    Goto done
  ${EndIf}

  DetailPrint "Removing start at sign-in, and noting that the ports were stopped."
  !insertmacro VssRunStopStudio "$INSTDIR\bin\stop-studio.ps1" "Forget" "$INSTDIR\forget-users.log"
  ${If} $0 == 0
    Delete "$INSTDIR\forget-users.log"
  ${Else}
    MessageBox MB_OK|MB_ICONEXCLAMATION "Virtual Serial Studio is removed, but start at sign-in could not be cleared for every user (code $0). An entry that is left starts nothing, because the program is gone.$\r$\n$\r$\nWhat happened is in $INSTDIR\forget-users.log" /SD IDOK
  ${EndIf}

done:
  ; Not in CPack's list of installed files (VssInstallStopStudio puts it there), so
  ; the uninstaller would leave it, and the folder with it.
  Delete "$INSTDIR\bin\stop-studio.ps1"
  Pop $0
FunctionEnd

; ---- An upgrade: telling it, and taking back a cancelled one -------------------
;
; The installer of a newer version runs the uninstaller of the old one first, before
; its first page (CPack's template, with /VSSUPGRADE=1 added, see nsis-template.cmake).
; That uninstaller deletes the program, its own Uninstall.exe and its entry in
; Apps & features, and keeps the driver, the helper service, the PATH entry and the
; per-user entries, because the new installer sets them up again. If the person then
; cancels, or the install fails, nothing would set them up and nothing could remove
; them. So the old uninstaller leaves the scripts that remove them in a folder
; (undo-upgrade) in the install folder, and the new installer runs them if it does not
; get to the end. A finished install deletes the folder. (The uninstaller cannot delete
; itself when it runs with _?=, so Uninstall.exe stays until the new installer finishes,
; which replaces it, or takes the upgrade back, which deletes it.)
;
; If the installer is killed (a power cut, Task Manager) after that, the folder stays
; and the old version's entry in Apps & features is already gone, so the next install
; runs no uninstaller. VssAdoptUnfinishedUpgrade, called at the start of every install
; (nsis-template.cmake), takes a folder left like that as an unfinished upgrade, so
; that this install, if it is cancelled or fails, runs the clean-up from it, and if it
; finishes, deletes it.
;
; This works from the version that has these functions on. An upgrade from an older
; version, cancelled or failed, leaves the driver, the helper service and the PATH
; entry, because that version's uninstaller makes no stash.

; First in the uninstall: is this an upgrade? Only the switch says so. A switch that is
; not there, or has any other value, is a real uninstall, which removes everything.
Function un.VssReadUpgrade
  Push $0
  Push $1
  StrCpy $VssIsUpgrade ""
  ${un.GetParameters} $0
  ${un.GetOptions} $0 "/VSSUPGRADE=" $1
  ${If} $1 == "1"
    StrCpy $VssIsUpgrade 1
  ${EndIf}
  Pop $1
  Pop $0
FunctionEnd

; In an upgrade, before anything is taken away: keeps the scripts that remove what the
; upgrade leaves, for the new installer. The same scripts the real uninstall runs.
Function un.VssStashUndo
  ${If} $VssIsUpgrade != 1
    Return
  ${EndIf}
  RMDir /r "$INSTDIR\${VSS_UNDO_DIR}"
  CreateDirectory "$INSTDIR\${VSS_UNDO_DIR}\helper"
  CopyFiles /SILENT "$INSTDIR\bin\driver\remove-driver.ps1" "$INSTDIR\${VSS_UNDO_DIR}\remove-driver.ps1"
  CopyFiles /SILENT "$INSTDIR\bin\driver\helper\install-helper.ps1" "$INSTDIR\${VSS_UNDO_DIR}\helper\install-helper.ps1"
  CopyFiles /SILENT "$INSTDIR\bin\edit-path.ps1" "$INSTDIR\${VSS_UNDO_DIR}\edit-path.ps1"
  CopyFiles /SILENT "$INSTDIR\bin\stop-studio.ps1" "$INSTDIR\${VSS_UNDO_DIR}\stop-studio.ps1"
FunctionEnd

; At the start of every install (a call the template copy makes at `inst:`): a stash in
; the install folder that this install did not make is an upgrade that did not finish,
; so it is treated as one. $INSTDIR is the default folder, or the one of /D=, here.
Function VssAdoptUnfinishedUpgrade
  ${If} $VssUpgradeFolder == ""
  ${AndIf} ${FileExists} "$INSTDIR\${VSS_UNDO_DIR}\*.*"
    StrCpy $VssUpgradeFolder $INSTDIR
  ${EndIf}
FunctionEnd

; At the end of a finished install: the upgrade went through, so there is nothing to
; take back. The install folder is cleared too, in case the stash was not the one this
; install knew about.
Function VssUpgradeDone
  ${If} $VssUpgradeFolder != ""
    RMDir /r "$VssUpgradeFolder\${VSS_UNDO_DIR}"
    StrCpy $VssUpgradeFolder ""
  ${EndIf}
  RMDir /r "$INSTDIR\${VSS_UNDO_DIR}"
FunctionEnd

; The person cancelled, or the install failed, after the old version was uninstalled:
; Studio is gone, so what the old uninstaller kept for this installer goes too, by the
; scripts it left. It does nothing when there is no stash: this installer ran no
; uninstaller, or the old one was a build that makes none. A stash with some of the
; scripts missing (a build with no staged driver has no remove-driver.ps1) runs the
; ones that are there. It runs once. Each script writes its own log in the old install
; folder, and a log is kept only when its script failed, with the scripts, so that the
; person can run them by hand. When all three worked it also deletes Uninstall.exe: the
; uninstaller that ran with _?= could not delete itself, and has exited by now.
Function VssUndoUpgrade
  Push $0
  Push $1
  Push $R0
  Push $R1
  StrCpy $R0 $VssUpgradeFolder
  StrCpy $VssUpgradeFolder ""
  ${If} $R0 == ""
    Goto done
  ${EndIf}
  StrCpy $R1 "$R0\${VSS_UNDO_DIR}"
  ${IfNot} ${FileExists} "$R1\*.*"
    Goto done
  ${EndIf}

  DetailPrint "The upgrade did not finish. Taking the driver, the helper service and the PATH entry of the earlier version away."
  StrCpy $1 0

  ${If} ${FileExists} "$R1\remove-driver.ps1"
    ExecWait '"$WINDIR\Sysnative\cmd.exe" /C set "PSModulePath=" & "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$R1\remove-driver.ps1" > "$R0\driver-remove.log" 2>&1' $0
    ${If} $0 == 0
    ${OrIf} $0 == 10
      Delete "$R0\driver-remove.log"
      ${If} $0 == 10
        SetRebootFlag true
      ${EndIf}
    ${Else}
      StrCpy $1 1
    ${EndIf}
  ${EndIf}

  ${If} ${FileExists} "$R1\edit-path.ps1"
    ExecWait '"$WINDIR\Sysnative\cmd.exe" /C set "PSModulePath=" & "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$R1\edit-path.ps1" -Action Remove -Directory "$R0\bin" > "$R0\path-setup.log" 2>&1' $0
    ${If} $0 == 0
      Delete "$R0\path-setup.log"
      SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
    ${Else}
      StrCpy $1 1
    ${EndIf}
  ${EndIf}

  ${If} ${FileExists} "$R1\stop-studio.ps1"
    ExecWait '"$WINDIR\Sysnative\cmd.exe" /C set "PSModulePath=" & "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$R1\stop-studio.ps1" -Action Forget -Directory "$R0\bin" > "$R0\forget-users.log" 2>&1' $0
    ${If} $0 == 0
      Delete "$R0\forget-users.log"
    ${Else}
      StrCpy $1 1
    ${EndIf}
  ${EndIf}

  ${If} $1 == 0
    RMDir /r "$R1"
    ; Only when no Apps & features entry points at it (the old uninstaller removed it).
    ReadRegStr $0 SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${VSS_REGISTRY_KEY}" "UninstallString"
    ${If} $0 == ""
      Delete "$R0\Uninstall.exe"
    ${EndIf}
    RMDir "$R0"
  ${Else}
    MessageBox MB_OK|MB_ICONEXCLAMATION "The upgrade did not finish, and the earlier version of Virtual Serial Studio was already removed. Taking its driver, helper service or PATH entry away did not finish.$\r$\n$\r$\nThe logs are in $R0 (driver-remove.log, path-setup.log, forget-users.log; the ones that are not there worked). The scripts that remove them are in $R1; run them as administrator." /SD IDOK
  ${EndIf}

done:
  Pop $R1
  Pop $R0
  Pop $1
  Pop $0
FunctionEnd

; MUI calls this when the person confirmed that they cancel; .onInstFailed is called
; when an install that began fails (a section aborts, a file cannot be written).
Function VssOnUserAbort
  Call VssUndoUpgrade
FunctionEnd

Function .onInstFailed
  Call VssUndoUpgrade
FunctionEnd
