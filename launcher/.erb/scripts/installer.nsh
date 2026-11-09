!include MUI2.nsh
!include nsDialogs.nsh
!include StdUtils.nsh
!include FileFunc.nsh

; https://github.com/electron-userland/electron-builder/tree/master/packages/app-builder-lib/templates/nsis
; if future changes need to be made, this is an important reference for what options electron-builder provides
; because their docs aren't all encompassing.

!define GC_INSTALLER "gc-driver-install.exe"

; The shortcuts. electron-builder.json sets createDesktopShortcut and createStartMenuShortcut to
; false, so electron-builder makes none: customInstall makes them (same paths, target and
; AppUserModelID as electron-builder's) as chosen on the options page, and customUnInstall
; removes them on a real uninstall.
!define DESKTOP_LINK "$DESKTOP\${SHORTCUT_NAME}.lnk"
!define START_MENU_LINK "$SMPROGRAMS\${SHORTCUT_NAME}.lnk"
!define SLDF_FORCE_NO_LINKTRACK 0x40000

; The result of whether we should install drivers or not
var InstallType
; 1 or 0: whether to create the desktop / Start Menu shortcut (default 1; the switches
; /NODESKTOPSHORTCUT and /NOSTARTMENUSHORTCUT, e.g. for a silent install, set 0)
var CreateDesktopShortcut
var CreateStartMenuShortcut
var DesktopShortcutCheckbox
var StartMenuShortcutCheckbox

!macro customHeader
  ShowInstDetails show
  ShowUninstDetails show

  !ifndef BUILD_UNINSTALLER
    !insertmacro keepPlayerFilesFunctions
  !endif
!macroend

; The player's own files in the install folder (a Brawl disc image, textures, a whole games folder
; picked as the install folder). Every update, and an install over an existing one, runs the old
; version's uninstaller, which deletes the install folder as a whole (electron-builder's
; uninstaller.nsh: every entry moved into its temp folder, then RMDir /r $INSTDIR), so a player's
; disc image put next to the launcher was gone after an update (Discord, 2026-11-09: "MY BRAWL ISO
; DELETED WHILE THE LAUNCHER HAS UPDATED"). The installer moves every top-level entry the launcher
; does not ship out of the old install folder into a folder next to it (the same drive, so even a
; disc image moves at once) before it runs the old uninstaller, and moves them back once the new
; version's files are in. This is the installer of the new version, so it keeps them on the update
; that ships it, whatever version is installed. An entry it cannot move stays where it is (and is
; deleted, as before); one it cannot move back (a launcher file of the same name) stays in the
; folder next to it, which is then left in place.
!define KEEP_DIR_SUFFIX ".kept-by-update"

; Sets VAR to 1 if NAME, a top-level entry of the install folder, is one the launcher ships (Electron's
; files, electron-builder's uninstaller), else 0. Any .dll, .pak, .bin or .dat counts as the launcher's,
; so files a later Electron adds are not kept across updates.
!macro isLauncherEntry NAME VAR
  StrCpy ${VAR} 0
  StrCpy $R9 "${NAME}" "" -4
  ${If} "${NAME}" == "${APP_EXECUTABLE_FILENAME}"
  ${OrIf} "${NAME}" == "${UNINSTALL_FILENAME}"
  ${OrIf} "${NAME}" == "uninstallerIcon.ico"
  ${OrIf} "${NAME}" == "locales"
  ${OrIf} "${NAME}" == "resources"
  ${OrIf} "${NAME}" == "LICENSE.electron.txt"
  ${OrIf} "${NAME}" == "LICENSES.chromium.html"
  ${OrIf} "${NAME}" == "vk_swiftshader_icd.json"
  ${OrIf} $R9 == ".dll"
  ${OrIf} $R9 == ".pak"
  ${OrIf} $R9 == ".bin"
  ${OrIf} $R9 == ".dat"
    StrCpy ${VAR} 1
  ${EndIf}
!macroend

!macro keepPlayerFilesFunctions
  ; The install folder the old version's uninstaller deletes, and the folder next to it that holds
  ; the player's files meanwhile; empty while nothing was moved.
  Var KeepFrom
  Var KeepDir

  ; Moves the player's files out of the installed version's folder (see KEEP_DIR_SUFFIX). Does
  ; nothing when no version is installed, or when it is installed at a drive's root.
  Function keepPlayerFiles
    Push $R0
    Push $R1
    Push $R2
    Push $R3
    Push $R9
    StrCpy $KeepFrom ""
    StrCpy $KeepDir ""
    ReadRegStr $R0 SHELL_CONTEXT "${UNINSTALL_REGISTRY_KEY}" UninstallString
    ${If} $R0 != ""
      ReadRegStr $R0 SHELL_CONTEXT "${INSTALL_REGISTRY_KEY}" InstallLocation
      ${If} $R0 == ""
        StrCpy $R0 $INSTDIR
      ${EndIf}
      StrCpy $R1 $R0 1 -1
      ${If} $R1 == "\"
        StrCpy $R0 $R0 -1
      ${EndIf}
      StrCpy $R1 $R0 1 -1
      ${If} $R1 != ":"
      ${AndIf} $R0 != ""
      ${AndIf} ${FileExists} "$R0\*.*"
        StrCpy $KeepFrom $R0
        StrCpy $KeepDir "$R0${KEEP_DIR_SUFFIX}"
        FindFirst $R1 $R2 "$KeepFrom\*.*"
        ${DoWhile} $R2 != ""
          ${If} $R2 != "."
          ${AndIf} $R2 != ".."
            !insertmacro isLauncherEntry $R2 $R3
            ${If} $R3 == 0
              CreateDirectory "$KeepDir"
              ClearErrors
              Rename "$KeepFrom\$R2" "$KeepDir\$R2"
              ${If} ${Errors}
                DetailPrint `Could not move "$KeepFrom\$R2" out of the way of the update.`
              ${Else}
                DetailPrint `Keeping "$R2" (not a ${PRODUCT_NAME} file) in "$KeepDir" during the update.`
              ${EndIf}
            ${EndIf}
          ${EndIf}
          FindNext $R1 $R2
        ${Loop}
        FindClose $R1
        ClearErrors
      ${EndIf}
    ${EndIf}
    Pop $R9
    Pop $R3
    Pop $R2
    Pop $R1
    Pop $R0
  FunctionEnd

  ; Moves the files keepPlayerFiles moved back into the install folder (made again if the old
  ; uninstaller removed it and nothing was installed there). Safe to call more than once.
  Function restorePlayerFiles
    ${If} $KeepDir != ""
    ${AndIf} ${FileExists} "$KeepDir\*.*"
      Push $R1
      Push $R2
      CreateDirectory "$KeepFrom"
      FindFirst $R1 $R2 "$KeepDir\*.*"
      ${DoWhile} $R2 != ""
        ${If} $R2 != "."
        ${AndIf} $R2 != ".."
          ${If} ${FileExists} "$KeepFrom\$R2"
            DetailPrint `"$R2" is a ${PRODUCT_NAME} file now: yours stays in "$KeepDir".`
          ${Else}
            ClearErrors
            Rename "$KeepDir\$R2" "$KeepFrom\$R2"
            ${If} ${Errors}
              DetailPrint `Could not move "$R2" back: it is in "$KeepDir".`
            ${EndIf}
          ${EndIf}
        ${EndIf}
        FindNext $R1 $R2
      ${Loop}
      FindClose $R1
      ClearErrors
      ; Only when empty.
      RMDir "$KeepDir"
      ClearErrors
      Pop $R2
      Pop $R1
    ${EndIf}
  FunctionEnd

  ; An install that failed after keepPlayerFiles (the old uninstaller removed the old version, then
  ; the new files could not be written) still puts the player's files back.
  Function .onInstFailed
    Call restorePlayerFiles
  FunctionEnd
!macroend

; Force-closes this user's running launchers (from any folder) and gives their files a moment.
!macro killLauncher
  Push $0
  nsExec::Exec `"$SYSDIR\cmd.exe" /C taskkill /F /IM "${APP_EXECUTABLE_FILENAME}" /FI "USERNAME eq %USERNAME%"`
  Pop $0
  Pop $0
  Sleep 1000
!macroend

; Sets VAR to 1 while this user runs a launcher (from any folder), else 0.
!macro launcherRunning VAR
  nsExec::Exec `"$SYSDIR\cmd.exe" /C tasklist /FI "USERNAME eq %USERNAME%" /FI "IMAGENAME eq ${APP_EXECUTABLE_FILENAME}" /FO CSV /NH | "$SYSDIR\findstr.exe" /B /I /C:"\"${APP_EXECUTABLE_FILENAME}\""`
  Pop ${VAR}
  ${If} ${VAR} == 0
    StrCpy ${VAR} 1
  ${Else}
    StrCpy ${VAR} 0
  ${EndIf}
!macroend

; Replaces electron-builder's check that the launcher is closed before an install or uninstall. A
; fresh install or an uninstall asks to close it, as before; an update gives the launcher that started
; it a moment to quit by itself. Then any launcher still or again running is closed, for up to about
; 20 s. electron-builder's check gave up when it still saw one right after its second force-close (a
; launcher opened again by a player who saw it vanish, or one not done exiting) and so quit the
; update, leaving the old version (bug report #219).
!macro customCheckAppRunning
  Push $R1
  Push $R2
  StrCpy $R1 0
  customCheckAppRunning_loop:
    !insertmacro launcherRunning $R2
    ${If} $R2 == 1
      IntOp $R1 $R1 + 1
      ${If} $R1 == 1
        ${If} ${isUpdated}
          ; The launcher that started the update is quitting by itself.
          Sleep 1500
        ${Else}
          MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION "$(appRunning)" /SD IDOK IDOK customCheckAppRunning_loop
          Quit
        ${EndIf}
      ${ElseIf} $R1 > 20
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "$(appCannotBeClosed)" /SD IDCANCEL IDRETRY customCheckAppRunning_retry
        Quit
        customCheckAppRunning_retry:
        StrCpy $R1 1
      ${Else}
        DetailPrint "$(appClosing)"
        !insertmacro killLauncher
      ${EndIf}
      Goto customCheckAppRunning_loop
    ${EndIf}
  Pop $R2
  Pop $R1

  ; The launcher is closed: the player's files can be moved out of the old version's way. (An
  ; elevated installer, which skips this check, does it in customInit.)
  !ifndef BUILD_UNINSTALLER
    Call keepPlayerFiles
  !endif
!macroend

!macro customInit
  ; Force silent mode for updates to make them seamless
  ; while still allowing directory selection for fresh installs
  ${If} ${isUpdated}
    SetSilent silent
    ; A silent update showed nothing for its 10-30 s, so players took the launcher for crashed and
    ; opened it again, and a launcher opened during the update made it fail (bug report #219).
    ; The banner stays until the new launcher is started (customInstall) or the installer ends.
    ${IfNot} ${UAC_IsInnerInstance}
      Banner::show /set 76 "Updating ${PRODUCT_NAME}..." "It reopens when it is done."
    ${EndIf}
  ${EndIf}

  ; The elevated installer of a silent update (a per-machine install) skips customCheckAppRunning,
  ; so it moves the player's files out of the old version's way here. Only silent: a wizard could
  ; still be cancelled.
  ${If} ${UAC_IsInnerInstance}
  ${AndIf} ${Silent}
    Call keepPlayerFiles
  ${EndIf}

  StrCpy $CreateDesktopShortcut 1
  StrCpy $CreateStartMenuShortcut 1
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/NODESKTOPSHORTCUT" $R1
  ${IfNot} ${Errors}
    StrCpy $CreateDesktopShortcut 0
  ${EndIf}
  ${If} ${isNoDesktopShortcut} ; electron-builder's own --no-desktop-shortcut
    StrCpy $CreateDesktopShortcut 0
  ${EndIf}
  ClearErrors
  ${GetOptions} $R0 "/NOSTARTMENUSHORTCUT" $R1
  ${IfNot} ${Errors}
    StrCpy $CreateStartMenuShortcut 0
  ${EndIf}
  ClearErrors
!macroend

!macro customPageAfterChangeDir
  Page custom InstTypePageCreate InstTypePageLeave
  Function InstTypePageCreate
    ${If} ${isUpdated}
      Abort
    ${EndIf}
    !insertmacro MUI_HEADER_TEXT "Select components to install" "Choose the optional drivers and shortcuts."
    nsDialogs::Create /NOUNLOAD 1018
    Pop $0
    ${NSD_CreateRadioButton} 0 50u 100% 10u "Only install ${PRODUCT_NAME}"
    pop $1
    ${NSD_CreateRadioButton} 0 70u 100% 10u "Also install GameCube adapter drivers (optional)"
    pop $2
    ${If} $InstallType == INSTALL
        ${NSD_Check} $2 ; Select install drivers
    ${Else}
        ${NSD_Check} $1 ; Select skip by default
    ${EndIf}
    ${NSD_CreateLabel} 0 0 100% 45u "Would you like to also install GameCube adapter drivers? This would allow you to use GameCube controllers with a compatible adapter (in Switch/Wii U mode) on your PC. If the adapter already has its driver (for example from Slippi), it is left as it is, overclock included."
    pop $3
    ${NSD_CreateCheckbox} 0 95u 100% 10u "Create a desktop shortcut"
    Pop $DesktopShortcutCheckbox
    ${If} $CreateDesktopShortcut == 1
      ${NSD_Check} $DesktopShortcutCheckbox
    ${EndIf}
    ${NSD_CreateCheckbox} 0 110u 100% 10u "Create a Start Menu shortcut"
    Pop $StartMenuShortcutCheckbox
    ${If} $CreateStartMenuShortcut == 1
      ${NSD_Check} $StartMenuShortcutCheckbox
    ${EndIf}
    nsDialogs::Show
  FunctionEnd

  Function InstTypePageLeave
    ${NSD_GetState} $1 $0
    ${If} $0 = ${BST_CHECKED}
      ; Skip was selected
      StrCpy $InstallType SKIP
    ${Else}
      ${NSD_GetState} $2 $0
      ${If} $0 = ${BST_CHECKED}
        ; Install was selected
        StrCpy $InstallType INSTALL
      ${Else}
        ; Nothing was selected
      ${EndIf}
    ${EndIf}

    StrCpy $CreateDesktopShortcut 0
    ${NSD_GetState} $DesktopShortcutCheckbox $0
    ${If} $0 = ${BST_CHECKED}
      StrCpy $CreateDesktopShortcut 1
    ${EndIf}
    StrCpy $CreateStartMenuShortcut 0
    ${NSD_GetState} $StartMenuShortcutCheckbox $0
    ${If} $0 = ${BST_CHECKED}
      StrCpy $CreateStartMenuShortcut 1
    ${EndIf}
  FunctionEnd
!macroend

; Sets SLDF_FORCE_NO_LINKTRACK on a shortcut, so that Windows never re-targets it to wherever the
; exe was moved. An update moves the old install into $PLUGINSDIR\old-install before deleting it;
; a shortcut that the shell resolved meanwhile followed the exe there and was left dead.
!macro setShortcutNoLinkTrack LINK
  System::Call "ole32::CoCreateInstance(g '{00021401-0000-0000-C000-000000000046}', p 0, i 1, g '{000214F9-0000-0000-C000-000000000046}', *p .r1) i .r0"
  ${If} $0 = 0
    ; IShellLinkW -> IPersistFile
    System::Call "$1->0(g '{0000010b-0000-0000-C000-000000000046}', *p .r2) i .r0"
    ${If} $0 = 0
      ; IPersistFile::Load(path, STGM_READWRITE)
      System::Call "$2->5(w '${LINK}', i 2) i .r0"
      ${If} $0 = 0
        ; IShellLinkW -> IShellLinkDataList
        System::Call "$1->0(g '{45e2b4ae-b1c3-11d0-b92f-00a0c90312e1}', *p .r3) i .r0"
        ${If} $0 = 0
          ; GetFlags, SetFlags, then IPersistFile::Save(NULL, TRUE)
          System::Call "$3->6(*i .r4) i .r0"
          ${If} $0 = 0
            IntOp $4 $4 | ${SLDF_FORCE_NO_LINKTRACK}
            System::Call "$3->7(i r4) i .r0"
            System::Call "$2->6(p 0, i 1) i .r0"
          ${EndIf}
          System::Call "$3->2()"
        ${EndIf}
      ${EndIf}
      System::Call "$2->2()"
    ${EndIf}
    System::Call "$1->2()"
  ${EndIf}
!macroend

; (Re)writes a shortcut exactly as electron-builder's addStartMenuLink/addDesktopLink do (working
; directory $INSTDIR), plus SLDF_FORCE_NO_LINKTRACK.
!macro writeShortcut LINK
  SetOutPath $INSTDIR
  CreateShortCut "${LINK}" "$INSTDIR\${APP_EXECUTABLE_FILENAME}" "" "$INSTDIR\${APP_EXECUTABLE_FILENAME}" 0 "" "" "${APP_DESCRIPTION}"
  ClearErrors
  WinShell::SetLnkAUMI "${LINK}" "${APP_ID}"
  !insertmacro setShortcutNoLinkTrack "${LINK}"
  ClearErrors
!macroend

; Sets $R0 to 1 when a GameCube adapter (057E:0337) this PC has seen already runs on WinUSB, else 0.
; Windows keeps the Enum key of an adapter that is unplugged now.
!macro gcAdapterHasWinUSB
  Push $R1
  Push $R2
  Push $R3
  StrCpy $R0 0
  StrCpy $R1 0
  ${Do}
    EnumRegKey $R2 HKLM "SYSTEM\CurrentControlSet\Enum\USB\VID_057E&PID_0337" $R1
    ${If} $R2 == ""
      ${Break}
    ${EndIf}
    ReadRegStr $R3 HKLM "SYSTEM\CurrentControlSet\Enum\USB\VID_057E&PID_0337\$R2" "Service"
    ${If} $R3 == "WinUSB"
      StrCpy $R0 1
      ${Break}
    ${EndIf}
    IntOp $R1 $R1 + 1
  ${Loop}
  ClearErrors
  Pop $R3
  Pop $R2
  Pop $R1
!macroend

!macro customInstall
  ; The player's files moved out of the old version's way go back next to the new files.
  Call restorePlayerFiles

  ; Check if we should also install the GC drivers
  ${If} $InstallType == INSTALL
    ; gc-driver-install.exe (libwdi, Zadig's engine) reinstalls WinUSB on the adapter, and a driver
    ; install drops the device's filter drivers: that takes away HIDUSBF, the 1000 Hz adapter
    ; overclock most players with a Slippi setup have ("Rolling back everything" in the overclock
    ; guide is exactly this). An adapter that already runs on WinUSB needs nothing, so it is left alone.
    Push $R0
    !insertmacro gcAdapterHasWinUSB
    ${If} $R0 == 1
      DetailPrint "The GameCube adapter already uses WinUSB, so its driver is left as it is."
    ${Else}
      ; Automatically run gamecube adapter driver installer
      File /oname=$PLUGINSDIR\${GC_INSTALLER} "${BUILD_RESOURCES_DIR}\${GC_INSTALLER}"
      ExecShellWait "" "$PLUGINSDIR\${GC_INSTALLER}" SW_HIDE
    ${EndIf}
    Pop $R0
  ${EndIf}

  ${If} ${isUpdated}
    ; An update never adds or removes shortcuts (the old uninstaller kept them: --keep-shortcuts),
    ; but re-points the ones the user has at the new files, in case one followed the old files
    ; into $PLUGINSDIR\old-install.
    ${If} ${FileExists} "${DESKTOP_LINK}"
      !insertmacro writeShortcut "${DESKTOP_LINK}"
    ${EndIf}
    ${If} ${FileExists} "${START_MENU_LINK}"
      !insertmacro writeShortcut "${START_MENU_LINK}"
    ${EndIf}
    ; A launcher opened during the update holds the single-instance lock, so the new launcher
    ; started next would quit at once and leave that one running (bug report #219).
    ${If} ${isForceRun}
      !insertmacro killLauncher
    ${EndIf}
    Banner::destroy
  ${Else}
    ${If} $CreateDesktopShortcut == 1
      !insertmacro writeShortcut "${DESKTOP_LINK}"
    ${Else}
      WinShell::UninstShortcut "${DESKTOP_LINK}"
      Delete "${DESKTOP_LINK}"
    ${EndIf}
    ${If} $CreateStartMenuShortcut == 1
      !insertmacro writeShortcut "${START_MENU_LINK}"
      StrCpy $launchLink "${START_MENU_LINK}"
    ${Else}
      WinShell::UninstShortcut "${START_MENU_LINK}"
      Delete "${START_MENU_LINK}"
    ${EndIf}
  ${EndIf}
  System::Call 'Shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'
!macroend

; electron-builder's check of the old version's uninstall (handleUninstallResult), plus: when an
; update's uninstall failed because the launcher was opened again meanwhile (its files in use, so
; they could not be moved), the launcher is closed and the uninstall tried again, twice, instead of
; quitting and leaving the old version in place (bug report #219: "I can't update the launcher").
!macro customUnInstallCheck
  Var /GLOBAL unInstallRetries
  StrCpy $unInstallRetries 0
  customUnInstallCheck_check:
  IfErrors 0 customUnInstallCheck_launched
    DetailPrint `Uninstall was not successful. Not able to launch uninstaller!`
    Return
  customUnInstallCheck_launched:
  ${If} $R0 != 0
    ${If} ${isUpdated}
    ${AndIf} $unInstallRetries < 2
      IntOp $unInstallRetries $unInstallRetries + 1
      DetailPrint `The old version's files are in use: closing ${PRODUCT_NAME} and trying again.`
      !insertmacro killLauncher
      Push SHELL_CONTEXT
      Call uninstallOldVersion
      Goto customUnInstallCheck_check
    ${EndIf}
    MessageBox MB_OK|MB_ICONEXCLAMATION "$(uninstallFailed): $R0" /SD IDOK
    DetailPrint `Uninstall was not successful. Uninstaller error code: $R0.`
    Call restorePlayerFiles
    SetErrorLevel 2
    Quit
  ${EndIf}
!macroend

!macro customUnInstall
  ; A real uninstall removes the shortcuts; the uninstall an update runs (--updated,
  ; --keep-shortcuts) keeps them.
  ${IfNot} ${isUpdated}
  ${AndIfNot} ${isKeepShortcuts}
    WinShell::UninstShortcut "$oldDesktopLink"
    Delete "$oldDesktopLink"
    WinShell::UninstShortcut "$oldStartMenuLink"
    Delete "$oldStartMenuLink"
  ${EndIf}

  ; Updates run this silently (/S): /SD answers No, so an update never deletes the player's data.
  ; Spelled out because Yes takes the Dolphin User folder with it (a player report 2026-10-19 lost
  ; custom textures, Dolphin settings, hotkeys and controller settings).
  MessageBox MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION "Also delete your ${PRODUCT_NAME} data?$\r$\n$\r$\nThis deletes your Dolphin settings, hotkeys, controller settings, custom textures, Project+ save and SD card, and your login. Choose No to keep them for a reinstall." \
    /SD IDNO IDNO Done IDYES Accepted

  Accepted:
    RMDir /r "$APPDATA\${APP_FILENAME}"
    !ifdef APP_PRODUCT_FILENAME
      RMDir /r "$APPDATA\${APP_PRODUCT_FILENAME}"
    !endif
  Done:
!macroend
