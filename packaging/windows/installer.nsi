; RexPlayer NSIS Installer Script
!include "MUI2.nsh"

Name "RexPlayer"
OutFile "RexPlayer-0.1.0-Setup.exe"
InstallDir "$PROGRAMFILES64\RexPlayer"
InstallDirRegKey HKLM "Software\RexPlayer" "InstallDir"
RequestExecutionLevel admin

; UI
!define MUI_ABORTWARNING
!define MUI_ICON "..\..\assets\rexplayer.ico"
!define MUI_UNICON "..\..\assets\rexplayer.ico"

; Pages
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install"
    SetOutPath "$INSTDIR"

    ; Main executable
    File "..\..\build\Release\rexplayer.exe"

    ; Qt DLLs
    File "..\..\build\Release\Qt6Core.dll"
    File "..\..\build\Release\Qt6Gui.dll"
    File "..\..\build\Release\Qt6Widgets.dll"

    ; Default config
    File "..\..\config\default.toml"

    ; Create shortcuts
    CreateDirectory "$SMPROGRAMS\RexPlayer"
    CreateShortcut "$SMPROGRAMS\RexPlayer\RexPlayer.lnk" "$INSTDIR\rexplayer.exe"
    CreateShortcut "$DESKTOP\RexPlayer.lnk" "$INSTDIR\rexplayer.exe"

    ; Register uninstaller
    WriteUninstaller "$INSTDIR\uninstall.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "DisplayName" "RexPlayer"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "UninstallString" "$INSTDIR\uninstall.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "DisplayVersion" "0.1.0"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "Publisher" "RexPlayer Team"
    WriteRegStr HKLM "Software\RexPlayer" "InstallDir" "$INSTDIR"

    ; Register .apk file association
    WriteRegStr HKCR ".apk\OpenWithProgids" "RexPlayer.apk" ""
    WriteRegStr HKCR "RexPlayer.apk" "" "Android Package"
    WriteRegStr HKCR "RexPlayer.apk\shell\open\command" "" '"$INSTDIR\rexplayer.exe" "%1"'
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\rexplayer.exe"
    Delete "$INSTDIR\Qt6Core.dll"
    Delete "$INSTDIR\Qt6Gui.dll"
    Delete "$INSTDIR\Qt6Widgets.dll"
    Delete "$INSTDIR\default.toml"
    Delete "$INSTDIR\uninstall.exe"

    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\RexPlayer\RexPlayer.lnk"
    RMDir "$SMPROGRAMS\RexPlayer"
    Delete "$DESKTOP\RexPlayer.lnk"

    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer"
    DeleteRegKey HKLM "Software\RexPlayer"
    DeleteRegKey HKCR "RexPlayer.apk"
SectionEnd
