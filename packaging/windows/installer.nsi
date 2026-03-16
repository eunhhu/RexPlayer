; RexPlayer NSIS Installer Script
; Requires NSIS 3.x (https://nsis.sourceforge.io/)

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

; ---------------------------------------------------------------------------
; General
; ---------------------------------------------------------------------------
Name "RexPlayer"
OutFile "RexPlayer-0.1.0-Setup.exe"
Unicode True
InstallDir "$PROGRAMFILES64\RexPlayer"
InstallDirRegKey HKLM "Software\RexPlayer" "InstallDir"
RequestExecutionLevel admin

; Version information
VIProductVersion "0.1.0.0"
VIAddVersionKey "ProductName" "RexPlayer"
VIAddVersionKey "ProductVersion" "0.1.0"
VIAddVersionKey "CompanyName" "RexPlayer"
VIAddVersionKey "LegalCopyright" "Copyright 2026 RexPlayer contributors"
VIAddVersionKey "FileDescription" "RexPlayer Installer"
VIAddVersionKey "FileVersion" "0.1.0"

; ---------------------------------------------------------------------------
; Modern UI Configuration
; ---------------------------------------------------------------------------
!define MUI_ABORTWARNING
!define MUI_ICON "..\..\assets\icons\rexplayer.ico"
!define MUI_UNICON "..\..\assets\icons\rexplayer.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "..\..\assets\installer-sidebar.bmp"

; Welcome page
!insertmacro MUI_PAGE_WELCOME

; License page
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"

; Components page
!insertmacro MUI_PAGE_COMPONENTS

; Directory page
!insertmacro MUI_PAGE_DIRECTORY

; Install files page
!insertmacro MUI_PAGE_INSTFILES

; Finish page
!define MUI_FINISHPAGE_RUN "$INSTDIR\rexplayer.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch RexPlayer"
!insertmacro MUI_PAGE_FINISH

; Uninstaller pages
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; Language
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
; Installer Sections
; ---------------------------------------------------------------------------
Section "RexPlayer Core (required)" SecCore
    SectionIn RO ; Required, cannot be deselected

    ; Check for 64-bit OS
    ${IfNot} ${RunningX64}
        MessageBox MB_OK|MB_ICONSTOP "RexPlayer requires a 64-bit version of Windows."
        Abort
    ${EndIf}

    ; Check for Windows 10+
    ${If} ${AtLeastWin10}
    ${Else}
        MessageBox MB_OK|MB_ICONSTOP "RexPlayer requires Windows 10 or later."
        Abort
    ${EndIf}

    SetOutPath "$INSTDIR"

    ; Core files
    File "..\..\build\Release\rexplayer.exe"
    File "..\..\build\Release\*.dll"

    ; Qt runtime
    SetOutPath "$INSTDIR\platforms"
    File "..\..\build\Release\platforms\*.dll"

    SetOutPath "$INSTDIR\styles"
    File /nonfatal "..\..\build\Release\styles\*.dll"

    SetOutPath "$INSTDIR\imageformats"
    File /nonfatal "..\..\build\Release\imageformats\*.dll"

    ; Configuration template
    SetOutPath "$INSTDIR"
    File "..\..\config\default.toml"

    ; Icon
    File "..\..\assets\icons\rexplayer.ico"

    ; Write registry keys
    WriteRegStr HKLM "Software\RexPlayer" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "Software\RexPlayer" "Version" "0.1.0"

    ; Uninstaller
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    ; Add/Remove Programs entry
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "DisplayName" "RexPlayer"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "DisplayVersion" "0.1.0"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "Publisher" "RexPlayer"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "DisplayIcon" "$INSTDIR\rexplayer.ico"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "URLInfoAbout" "https://github.com/rexplayer/rexplayer"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "NoRepair" 1

    ; Calculate installed size
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer" \
        "EstimatedSize" "$0"
SectionEnd

Section "Desktop Shortcut" SecDesktop
    CreateShortCut "$DESKTOP\RexPlayer.lnk" "$INSTDIR\rexplayer.exe" "" \
        "$INSTDIR\rexplayer.ico" 0
SectionEnd

Section "Start Menu Shortcuts" SecStartMenu
    CreateDirectory "$SMPROGRAMS\RexPlayer"
    CreateShortCut "$SMPROGRAMS\RexPlayer\RexPlayer.lnk" "$INSTDIR\rexplayer.exe" "" \
        "$INSTDIR\rexplayer.ico" 0
    CreateShortCut "$SMPROGRAMS\RexPlayer\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "WHPX Hypervisor Check" SecWHPX
    ; Check if Windows Hypervisor Platform is enabled
    nsExec::ExecToStack 'powershell -Command "(Get-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform).State"'
    Pop $0 ; return code
    Pop $1 ; output

    ${If} $1 != "Enabled"
        MessageBox MB_YESNO|MB_ICONQUESTION \
            "Windows Hypervisor Platform (WHPX) is not enabled.$\n$\n\
             RexPlayer requires WHPX for hardware-accelerated virtualization.$\n$\n\
             Would you like to enable it now? (Requires reboot)" \
            IDYES enable_whpx IDNO skip_whpx

        enable_whpx:
            nsExec::ExecToStack 'powershell -Command "Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -NoRestart"'
            MessageBox MB_OK "WHPX has been enabled. Please restart your computer before using RexPlayer."

        skip_whpx:
    ${EndIf}
SectionEnd

; ---------------------------------------------------------------------------
; Section Descriptions
; ---------------------------------------------------------------------------
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} \
        "Core RexPlayer application files (required)."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} \
        "Create a shortcut on the Desktop."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} \
        "Create shortcuts in the Start Menu."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecWHPX} \
        "Check and optionally enable Windows Hypervisor Platform (WHPX)."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------------------
; Uninstaller
; ---------------------------------------------------------------------------
Section "Uninstall"
    ; Remove files
    Delete "$INSTDIR\rexplayer.exe"
    Delete "$INSTDIR\*.dll"
    Delete "$INSTDIR\default.toml"
    Delete "$INSTDIR\rexplayer.ico"
    Delete "$INSTDIR\Uninstall.exe"

    ; Remove subdirectories
    RMDir /r "$INSTDIR\platforms"
    RMDir /r "$INSTDIR\styles"
    RMDir /r "$INSTDIR\imageformats"
    RMDir "$INSTDIR"

    ; Remove shortcuts
    Delete "$DESKTOP\RexPlayer.lnk"
    Delete "$SMPROGRAMS\RexPlayer\RexPlayer.lnk"
    Delete "$SMPROGRAMS\RexPlayer\Uninstall.lnk"
    RMDir "$SMPROGRAMS\RexPlayer"

    ; Remove registry keys
    DeleteRegKey HKLM "Software\RexPlayer"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RexPlayer"
SectionEnd
