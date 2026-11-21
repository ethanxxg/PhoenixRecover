!include "MUI2.nsh"

!ifndef APP_VERSION
!define APP_VERSION "1.0.0.20260527"
!endif
!ifndef APP_NAME
!define APP_NAME "PhoenixRecover"
!endif
!ifndef APP_PUBLISHER
!define APP_PUBLISHER "PhoenixRecover Contributors"
!endif
!ifndef APP_EXE
!define APP_EXE "PhoenixRecover.exe"
!endif
!ifndef APP_SOURCE_DIR
!define APP_SOURCE_DIR "."
!endif
!ifndef APP_OUTPUT_DIR
!define APP_OUTPUT_DIR "."
!endif
!ifndef APP_INSTALL_DIR
!define APP_INSTALL_DIR "$PROGRAMFILES64\\${APP_NAME}"
!endif

Name "${APP_NAME} ${APP_VERSION}"
OutFile "${APP_OUTPUT_DIR}\\${APP_NAME}-Setup-${APP_VERSION}.exe"
InstallDir "${APP_INSTALL_DIR}"
InstallDirRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "InstallLocation"
RequestExecutionLevel admin
Unicode True

!define MUI_ABORTWARNING
!ifexist "${APP_SOURCE_DIR}\\resources\\icons\\app.ico"
  !define MUI_ICON "${APP_SOURCE_DIR}\\resources\\icons\\app.ico"
  !define MUI_UNICON "${APP_SOURCE_DIR}\\resources\\icons\\app.ico"
!endif

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${APP_SOURCE_DIR}\\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Section "MainSection" SEC01
  SetOverwrite on
  SetOutPath "$INSTDIR"
  File /r /x "PhoenixRecover-Setup-*.exe" /x "*.log" "${APP_SOURCE_DIR}\\*.*"

  CreateDirectory "$SMPROGRAMS\\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\\${APP_NAME}\\${APP_NAME}.lnk" "$INSTDIR\\${APP_EXE}"
  CreateShortcut "$DESKTOP\\${APP_NAME}.lnk" "$INSTDIR\\${APP_EXE}"

  WriteUninstaller "$INSTDIR\\Uninstall.exe"

  WriteRegStr HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${APP_NAME}" "DisplayName" "${APP_NAME} ${APP_VERSION}"
  WriteRegStr HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${APP_NAME}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${APP_NAME}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${APP_NAME}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${APP_NAME}" "UninstallString" "$\"$INSTDIR\\Uninstall.exe$\""
  WriteRegDWORD HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${APP_NAME}" "NoModify" 1
  WriteRegDWORD HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${APP_NAME}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\\${APP_NAME}\\${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\\${APP_NAME}"

  ; Remove only files installed by PhoenixRecover. Do not recursively remove
  ; the install directory: crash_reports and user-created files must survive.
  Delete "$INSTDIR\\PhoenixRecover.exe"
  Delete "$INSTDIR\\*.dll"
  Delete "$INSTDIR\\Uninstall.exe"
  Delete "$INSTDIR\\LICENSE"
  Delete "$INSTDIR\\README.md"
  Delete "$INSTDIR\\VERSION"
  Delete "$INSTDIR\\PhoenixRecover-Setup-*.exe"
  RMDir /r "$INSTDIR\\platforms"
  RMDir /r "$INSTDIR\\styles"
  RMDir /r "$INSTDIR\\imageformats"
  RMDir /r "$INSTDIR\\iconengines"
  RMDir /r "$INSTDIR\\docs"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${APP_NAME}"
SectionEnd
