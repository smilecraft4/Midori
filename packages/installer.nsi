; NSIS installer for Midori. Invoked by the midori_installer CMake target with NSIS_CONFIG_PATH, NSIS_BIN_DIR and
; NSIS_OUT_FILE defined on the command line.

!include "MUI2.nsh"
!include "${NSIS_CONFIG_PATH}"

Unicode true
Name "${APP_NAME}"
BrandingText "${APP_COPYRIGHT} ${APP_COMPANY}"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "${APP_REGKEY}" "InstallPath"
RequestExecutionLevel admin
OutFile "${NSIS_OUT_FILE}"
ShowInstDetails show
ShowUninstDetails show

Var StartMenuFolder

!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "${SOURCE_DIR}\packages\header.bmp"
!define MUI_HEADERIMAGE_RIGHT
!define MUI_WELCOMEFINISHPAGE_BITMAP "${SOURCE_DIR}\packages\wizard.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${SOURCE_DIR}\packages\wizard.bmp"
!define MUI_WELCOMEPAGE_TEXT "Setup will guide you through the installation of ${APP_NAME}.$\r$\n$\r$\nPlease close any running instances of ${APP_NAME} before continuing.$\r$\n$\r$\nClick Next to continue."

!define MUI_ICON "${ICON_FILE}"
!define MUI_UNICON "${ICON_FILE}"

!define MUI_STARTMENUPAGE_DEFAULTFOLDER "${APP_NAME}"
!define MUI_STARTMENUPAGE_REGISTRY_ROOT "HKLM"
!define MUI_STARTMENUPAGE_REGISTRY_KEY "${APP_REGKEY}"
!define MUI_STARTMENUPAGE_REGISTRY_VALUENAME "StartMenuFolder"

!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_STARTMENU Application $StartMenuFolder
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "FileVersion" "${APP_VERSION}.0"
VIAddVersionKey "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "CompanyName" "${APP_COMPANY}"
VIAddVersionKey "LegalCopyright" "${APP_COPYRIGHT} ${APP_COMPANY}"

!insertmacro MUI_LANGUAGE "English"

Function .onInit
    SetShellVarContext all
FunctionEnd

Function un.onInit
    SetShellVarContext all
FunctionEnd

Section "-Application Files" SecCore
    SectionIn RO

    SetOutPath "$INSTDIR"
    File "${NSIS_BIN_DIR}\${APP_EXE}"

    SetOutPath "$INSTDIR\lang"
    File "${SOURCE_DIR}\data\lang\*.json"

    SetOutPath "$INSTDIR\brushes"
    File "${SOURCE_DIR}\data\brushes\round.mdsl"

    SetOutPath "$INSTDIR"
    WriteUninstaller "$INSTDIR\${APP_UNINST}"

    WriteRegStr HKLM "${APP_REGKEY}" "" "$INSTDIR"
    WriteRegStr HKLM "${APP_REGKEY}" "Version" "${APP_VERSION}"
    WriteRegStr HKLM "${APP_REGKEY}" "InstallPath" "$INSTDIR"

    WriteRegStr HKLM "${APP_UNINST_KEY}" "DisplayName" "${APP_NAME} ${APP_VERSION}"
    WriteRegStr HKLM "${APP_UNINST_KEY}" "UninstallString" "$\"$INSTDIR\${APP_UNINST}$\""
    WriteRegStr HKLM "${APP_UNINST_KEY}" "QuietUninstallString" "$\"$INSTDIR\${APP_UNINST}$\" /S"
    WriteRegStr HKLM "${APP_UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${APP_UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKLM "${APP_UNINST_KEY}" "Publisher" "${APP_COMPANY}"
    WriteRegStr HKLM "${APP_UNINST_KEY}" "DisplayIcon" "$\"$INSTDIR\${APP_EXE}$\",0"
    WriteRegDWORD HKLM "${APP_UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${APP_UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Start Menu Shortcuts" SecStartMenu
    !insertmacro MUI_STARTMENU_WRITE_BEGIN Application
    CreateDirectory "$SMPROGRAMS\$StartMenuFolder"
    CreateShortcut "$SMPROGRAMS\$StartMenuFolder\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
    CreateShortcut "$SMPROGRAMS\$StartMenuFolder\Uninstall ${APP_NAME}.lnk" "$INSTDIR\${APP_UNINST}"
    !insertmacro MUI_STARTMENU_WRITE_END
SectionEnd

Section /o "Desktop Shortcut" SecDesktop
    CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

Section "Associate .mori canvas files" SecAssoc
    WriteRegStr HKLM "${APP_ASSOC_KEY}" "" "${APP_PROGID}"
    WriteRegStr HKLM "${APP_PROGID_KEY}" "" "${APP_NAME} canvas"
    WriteRegStr HKLM "${APP_PROGID_KEY}\DefaultIcon" "" "$\"$INSTDIR\${APP_EXE}$\",0"
    WriteRegStr HKLM "${APP_PROGID_KEY}\shell\open\command" "" "$\"$INSTDIR\${APP_EXE}$\" $\"%1$\""
    System::Call 'Shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "Create shortcuts in the Start Menu."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Create a ${APP_NAME} shortcut on the Desktop."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecAssoc} "Open .mori canvas files with ${APP_NAME}."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
    Delete "$INSTDIR\${APP_EXE}"
    Delete "$INSTDIR\${APP_UNINST}"
    Delete "$INSTDIR\lang\*.json"
    RMDir "$INSTDIR\lang"
    Delete "$INSTDIR\brushes\round.mdsl"
    RMDir "$INSTDIR\brushes"
    RMDir "$INSTDIR"

    !insertmacro MUI_STARTMENU_GETFOLDER Application $StartMenuFolder
    StrCmp $StartMenuFolder "" +4
    Delete "$SMPROGRAMS\$StartMenuFolder\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\$StartMenuFolder\Uninstall ${APP_NAME}.lnk"
    RMDir "$SMPROGRAMS\$StartMenuFolder"

    Delete "$DESKTOP\${APP_NAME}.lnk"

    DeleteRegKey HKLM "${APP_PROGID_KEY}"
    ReadRegStr $0 HKLM "${APP_ASSOC_KEY}" ""
    StrCmp $0 "${APP_PROGID}" 0 +2
    DeleteRegKey HKLM "${APP_ASSOC_KEY}"
    System::Call 'Shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

    DeleteRegKey HKLM "${APP_UNINST_KEY}"
    DeleteRegKey HKLM "${APP_REGKEY}"

    SetAutoClose true
SectionEnd
