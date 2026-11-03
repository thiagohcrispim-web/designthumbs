; Instalador do DesignThumbs (por usuário, sem administrador).
; Compilar: makensis -DVERSION=8.0.0 installer.nsi
Unicode true
Target amd64-unicode ; 64 bits: registra a DLL na mesma visão do registro que o Explorer usa
!ifndef VERSION
  !define VERSION "8.0.0"
!endif
!define APP "DesignThumbs"
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\DesignThumbs"

Name "${APP}"
OutFile "DesignThumbs-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\DesignThumbs"
InstallDirRegKey HKCU "Software\DesignThumbs" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "${APP} ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "FileDescription" "Instalador do ${APP}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "Thiago Crispim"
VIAddVersionKey "LegalCopyright" "Thiago Crispim"

!include "MUI2.nsh"
!define MUI_ICON "DesignPeek.ico"
!define MUI_UNICON "DesignPeek.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\DesignPeek.exe"
!define MUI_FINISHPAGE_RUN_PARAMETERS "/settings"
!define MUI_FINISHPAGE_RUN_TEXT "$(RunText)"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "PortugueseBR"
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Spanish"

LangString RunText ${LANG_PORTUGUESEBR} "Abrir as configurações do DesignPeek"
LangString RunText ${LANG_ENGLISH} "Open DesignPeek settings"
LangString RunText ${LANG_SPANISH} "Abrir la configuración de DesignPeek"
LangString Restart ${LANG_PORTUGUESEBR} "Reiniciando o Explorer..."
LangString Restart ${LANG_ENGLISH} "Restarting File Explorer..."
LangString Restart ${LANG_SPANISH} "Reiniciando el Explorador..."

; Fecha tudo que pode estar usando a DLL ou o EXE (para poder substituir)
!macro StopAll
  ExecWait '"$INSTDIR\DesignPeek.exe" /quit'
  nsExec::Exec 'taskkill /f /im DesignPeek.exe'
  nsExec::Exec 'taskkill /f /im prevhost.exe'
  nsExec::Exec 'taskkill /f /im dllhost.exe'
  DetailPrint "$(Restart)"
  nsExec::Exec 'taskkill /f /im explorer.exe'
  Sleep 800
!macroend

Section "Install"
  SetOutPath "$INSTDIR"
  !insertmacro StopAll
  IfFileExists "$INSTDIR\DesignThumbs.dll" 0 +2
    ExecWait 'regsvr32 /u /s "$INSTDIR\DesignThumbs.dll"'
  File "DesignThumbs.dll"
  File "DesignPeek.exe"
  File "DesignPeek.ico"
  File "diagnostico.bat"
  File "README.md"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ExecWait 'regsvr32 /s "$INSTDIR\DesignThumbs.dll"'
  ExecWait '"$INSTDIR\DesignPeek.exe" /autostart'          ; Inicializar, menu Iniciar e menu Novo
  Delete "$LOCALAPPDATA\Microsoft\Windows\Explorer\thumbcache_*.db" ; miniaturas antigas
  Exec "explorer.exe"
  Sleep 1500

  WriteRegStr HKCU "Software\DesignThumbs" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST}" "Publisher" "Thiago Crispim"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\DesignPeek.exe"
  WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegStr HKCU "${UNINST}" "URLInfoAbout" "https://github.com/thiagohcrispim-web/designthumbs"
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNINST}" "EstimatedSize" 1100

  ; atualização silenciosa (/S): volta a abrir o DesignPeek sozinho
  IfSilent 0 +2
    Exec '"$INSTDIR\DesignPeek.exe"'
SectionEnd

Function .onInit
  ; idioma do instalador = idioma do Windows (sem perguntar)
  System::Call 'kernel32::GetUserDefaultUILanguage() i .r0'
  IntOp $0 $0 & 0x3FF
  StrCmp $0 22 0 +3 ; LANG_PORTUGUESE
    StrCpy $LANGUAGE ${LANG_PORTUGUESEBR}
    Goto done
  StrCmp $0 10 0 +3 ; LANG_SPANISH
    StrCpy $LANGUAGE ${LANG_SPANISH}
    Goto done
  StrCpy $LANGUAGE ${LANG_ENGLISH}
  done:
FunctionEnd

Section "Uninstall"
  ExecWait '"$INSTDIR\DesignPeek.exe" /quit'
  ExecWait '"$INSTDIR\DesignPeek.exe" /noautostart'
  ExecWait 'regsvr32 /u /s "$INSTDIR\DesignThumbs.dll"'
  nsExec::Exec 'taskkill /f /im DesignPeek.exe'
  nsExec::Exec 'taskkill /f /im prevhost.exe'
  nsExec::Exec 'taskkill /f /im dllhost.exe'
  nsExec::Exec 'taskkill /f /im explorer.exe'
  Sleep 800
  Delete "$INSTDIR\DesignThumbs.dll"
  Delete "$INSTDIR\DesignPeek.exe"
  Delete "$INSTDIR\DesignPeek.ico"
  Delete "$INSTDIR\diagnostico.bat"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Exec "explorer.exe"
  DeleteRegKey HKCU "${UNINST}"
  DeleteRegValue HKCU "Software\DesignThumbs" "InstallDir"
SectionEnd

Function un.onInit
  System::Call 'kernel32::GetUserDefaultUILanguage() i .r0'
  IntOp $0 $0 & 0x3FF
  StrCmp $0 22 0 +3
    StrCpy $LANGUAGE ${LANG_PORTUGUESEBR}
    Goto done
  StrCmp $0 10 0 +3
    StrCpy $LANGUAGE ${LANG_SPANISH}
    Goto done
  StrCpy $LANGUAGE ${LANG_ENGLISH}
  done:
FunctionEnd
