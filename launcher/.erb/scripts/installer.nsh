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
!macroend

!macro customInit
  ; Force silent mode for updates to make them seamless
  ; while still allowing directory selection for fresh installs
  ${If} ${isUpdated}
    SetSilent silent
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
    ${NSD_CreateLabel} 0 0 100% 30u "Would you like to also install GameCube adapter drivers? This would allow you to use GameCube controllers with a compatible adapter (in Switch/Wii U mode) on your PC. Skip this if you already have GameCube adapter drivers installed."
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

!macro customInstall
  ; Check if we should also install the GC drivers
  ${If} $InstallType == INSTALL
    ; Automatically run gamecube adapter driver installer
    File /oname=$PLUGINSDIR\${GC_INSTALLER} "${BUILD_RESOURCES_DIR}\${GC_INSTALLER}"
    ExecShellWait "" "$PLUGINSDIR\${GC_INSTALLER}" SW_HIDE
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

  MessageBox MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION "Would you like to also clear ${PRODUCT_NAME} application data (including its Dolphin settings)?" \
    /SD IDNO IDNO Done IDYES Accepted

  Accepted:
    RMDir /r "$APPDATA\${APP_FILENAME}"
    !ifdef APP_PRODUCT_FILENAME
      RMDir /r "$APPDATA\${APP_PRODUCT_FILENAME}"
    !endif
  Done:
!macroend
