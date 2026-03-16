#!/usr/bin/env bash
# package.sh — Package RexPlayer for distribution
#
# Creates platform-specific distribution packages:
#   Linux  : AppImage
#   macOS  : DMG (with Applications symlink)
#   Windows: NSIS installer configuration
#
# Usage:
#   ./scripts/package.sh [--build-dir DIR] [--output DIR] [--version VER]

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build}"
OUTPUT_DIR="${OUTPUT_DIR:-${PROJECT_ROOT}/dist}"
DEFAULT_VERSION="$(
    sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' \
        "${PROJECT_ROOT}/CMakeLists.txt" | head -n 1
)"
VERSION="${VERSION:-${DEFAULT_VERSION:-0.1.0}}"
APP_NAME="RexPlayer"

# ── Argument parsing ─────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir|-b) BUILD_DIR="$2";  shift 2 ;;
        --output|-o)    OUTPUT_DIR="$2"; shift 2 ;;
        --version|-v)   VERSION="$2";   shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--build-dir DIR] [--output DIR] [--version VER]"
            exit 0 ;;
        *)
            echo "Error: unknown argument '$1'" >&2; exit 1 ;;
    esac
done

echo "══════════════════════════════════════════════════════════════"
echo "  ${APP_NAME} Packager v${VERSION}"
echo "══════════════════════════════════════════════════════════════"

mkdir -p "${OUTPUT_DIR}"

# ── Platform detection ────────────────────────────────────────────────
detect_platform() {
    local os
    os="$(uname -s)"
    case "${os}" in
        Linux)                   echo "linux"   ;;
        Darwin)                  echo "macos"   ;;
        MINGW*|MSYS*|CYGWIN*)   echo "windows" ;;
        *)
            echo "Error: unsupported OS '${os}'" >&2
            exit 1
            ;;
    esac
}

PLATFORM="$(detect_platform)"
echo "  Platform   : ${PLATFORM}"
echo "  Build dir  : ${BUILD_DIR}"
echo "  Output dir : ${OUTPUT_DIR}"
echo ""

# ══════════════════════════════════════════════════════════════════════
# Linux — AppImage
# ══════════════════════════════════════════════════════════════════════
package_linux() {
    echo "[Linux] Creating AppImage..."

    local APPDIR="${BUILD_DIR}/${APP_NAME}.AppDir"
    rm -rf "${APPDIR}"
    mkdir -p "${APPDIR}/usr/bin"
    mkdir -p "${APPDIR}/usr/lib"
    mkdir -p "${APPDIR}/usr/share/applications"
    mkdir -p "${APPDIR}/usr/share/icons/hicolor/256x256/apps"

    # Copy the main binary
    if [[ ! -f "${BUILD_DIR}/rexplayer" ]]; then
        echo "Error: binary not found at ${BUILD_DIR}/rexplayer" >&2
        echo "  Build the project first: cmake --build build" >&2
        exit 1
    fi
    cp "${BUILD_DIR}/rexplayer" "${APPDIR}/usr/bin/"

    # Copy shared libraries that we built
    find "${BUILD_DIR}" -name "*.so" -exec cp {} "${APPDIR}/usr/lib/" \; 2>/dev/null || true

    # Desktop entry
    cat > "${APPDIR}/usr/share/applications/${APP_NAME}.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=${APP_NAME}
Exec=rexplayer
Icon=rexplayer
Categories=Development;Emulator;
Comment=Lightweight Android app player
DESKTOP

    # AppRun launcher — entry point for AppImage
    cat > "${APPDIR}/AppRun" <<'APPRUN'
#!/usr/bin/env bash
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="${SELF_DIR}/usr/lib:${LD_LIBRARY_PATH:-}"
exec "${SELF_DIR}/usr/bin/rexplayer" "$@"
APPRUN
    chmod +x "${APPDIR}/AppRun"

    # Symlinks required by AppImage spec
    ln -sf "usr/share/applications/${APP_NAME}.desktop" "${APPDIR}/${APP_NAME}.desktop"

    # Create the AppImage using appimagetool if available
    local OUTPUT_FILE="${OUTPUT_DIR}/${APP_NAME}-${VERSION}-x86_64.AppImage"
    if command -v appimagetool &>/dev/null; then
        ARCH=x86_64 appimagetool "${APPDIR}" "${OUTPUT_FILE}"
        chmod +x "${OUTPUT_FILE}"
        echo "[Linux] AppImage created: ${OUTPUT_FILE}"
    else
        # Fallback: create a tarball of the AppDir
        echo "[Linux] appimagetool not found — creating tarball instead."
        OUTPUT_FILE="${OUTPUT_DIR}/${APP_NAME}-${VERSION}-linux-x86_64.tar.gz"
        tar -czf "${OUTPUT_FILE}" -C "${BUILD_DIR}" "${APP_NAME}.AppDir"
        echo "[Linux] Tarball created: ${OUTPUT_FILE}"
    fi
}

# ══════════════════════════════════════════════════════════════════════
# macOS — DMG
# ══════════════════════════════════════════════════════════════════════
package_macos() {
    echo "[macOS] Creating DMG..."

    local SOURCE_APP_BUNDLE="${BUILD_DIR}/rexplayer.app"
    local APP_BUNDLE="${BUILD_DIR}/packaging-${APP_NAME}.app"
    local ENTITLEMENTS_PLIST="${PROJECT_ROOT}/packaging/macos/entitlements.plist"

    rm -rf "${APP_BUNDLE}"

    if [[ -d "${SOURCE_APP_BUNDLE}" ]]; then
        cp -R "${SOURCE_APP_BUNDLE}" "${APP_BUNDLE}"
    elif [[ -f "${BUILD_DIR}/rexplayer" ]]; then
        mkdir -p "${APP_BUNDLE}/Contents/MacOS"
        mkdir -p "${APP_BUNDLE}/Contents/Resources"
        mkdir -p "${APP_BUNDLE}/Contents/Frameworks"
        cp "${BUILD_DIR}/rexplayer" "${APP_BUNDLE}/Contents/MacOS/rexplayer"
        cp "${PROJECT_ROOT}/packaging/macos/Info.plist" "${APP_BUNDLE}/Contents/Info.plist"
        find "${BUILD_DIR}" -name "*.dylib" -exec cp {} "${APP_BUNDLE}/Contents/Frameworks/" \; 2>/dev/null || true
    else
        echo "Error: neither ${SOURCE_APP_BUNDLE} nor ${BUILD_DIR}/rexplayer exists." >&2
        echo "  Build the project first: cmake --build build" >&2
        exit 1
    fi

    if [[ -f "${ENTITLEMENTS_PLIST}" ]] && command -v codesign &>/dev/null; then
        codesign --force --deep --sign - --entitlements "${ENTITLEMENTS_PLIST}" "${APP_BUNDLE}"
    fi

    # Create DMG staging directory
    local DMG_STAGING="${BUILD_DIR}/dmg-staging"
    rm -rf "${DMG_STAGING}"
    mkdir -p "${DMG_STAGING}"
    cp -R "${APP_BUNDLE}" "${DMG_STAGING}/"

    # Add Applications symlink for drag-and-drop install
    ln -s /Applications "${DMG_STAGING}/Applications"

    # Build the DMG
    local DMG_FILE="${OUTPUT_DIR}/${APP_NAME}-${VERSION}-macOS.dmg"
    rm -f "${DMG_FILE}"

    hdiutil create \
        -volname "${APP_NAME} ${VERSION}" \
        -srcfolder "${DMG_STAGING}" \
        -ov \
        -format UDZO \
        "${DMG_FILE}"

    echo "[macOS] DMG created: ${DMG_FILE}"

    # Cleanup staging
    rm -rf "${DMG_STAGING}"
}

# ══════════════════════════════════════════════════════════════════════
# Windows — NSIS installer config
# ══════════════════════════════════════════════════════════════════════
package_windows() {
    echo "[Windows] Generating NSIS installer config..."

    local NSIS_DIR="${BUILD_DIR}/nsis"
    mkdir -p "${NSIS_DIR}"

    # Generate the NSIS script
    local NSIS_FILE="${NSIS_DIR}/installer.nsi"
    cat > "${NSIS_FILE}" <<NSIS
; ${APP_NAME} NSIS Installer Script
; Generated by package.sh

!include "MUI2.nsh"

; ── General ───────────────────────────────────────────────────────────
Name "${APP_NAME}"
OutFile "${APP_NAME}-${VERSION}-Setup.exe"
InstallDir "\$PROGRAMFILES64\\\\${APP_NAME}"
InstallDirRegKey HKLM "Software\\\\${APP_NAME}" "InstallDir"
RequestExecutionLevel admin

; ── Version info ──────────────────────────────────────────────────────
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP_NAME} Installer"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2024-2026 Rex Team"

; ── Interface ─────────────────────────────────────────────────────────
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ── Install section ───────────────────────────────────────────────────
Section "Install"
    SetOutPath \$INSTDIR

    ; Main binary
    File "${APP_NAME}.exe"

    ; DLLs
    File /nonfatal "*.dll"

    ; Create uninstaller
    WriteUninstaller "\$INSTDIR\\\\Uninstall.exe"

    ; Start menu shortcuts
    CreateDirectory "\$SMPROGRAMS\\\\${APP_NAME}"
    CreateShortcut  "\$SMPROGRAMS\\\\${APP_NAME}\\\\${APP_NAME}.lnk" "\$INSTDIR\\\\${APP_NAME}.exe"
    CreateShortcut  "\$SMPROGRAMS\\\\${APP_NAME}\\\\Uninstall.lnk"   "\$INSTDIR\\\\Uninstall.exe"

    ; Registry (for Add/Remove Programs)
    WriteRegStr HKLM "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\${APP_NAME}" \\
                     "DisplayName" "${APP_NAME}"
    WriteRegStr HKLM "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\${APP_NAME}" \\
                     "UninstallString" "\$INSTDIR\\\\Uninstall.exe"
    WriteRegStr HKLM "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\${APP_NAME}" \\
                     "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "Software\\\\${APP_NAME}" "InstallDir" \$INSTDIR
SectionEnd

; ── Uninstall section ─────────────────────────────────────────────────
Section "Uninstall"
    Delete "\$INSTDIR\\\\${APP_NAME}.exe"
    Delete "\$INSTDIR\\\\*.dll"
    Delete "\$INSTDIR\\\\Uninstall.exe"

    RMDir /r "\$SMPROGRAMS\\\\${APP_NAME}"
    RMDir    "\$INSTDIR"

    DeleteRegKey HKLM "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\${APP_NAME}"
    DeleteRegKey HKLM "Software\\\\${APP_NAME}"
SectionEnd
NSIS

    echo "[Windows] NSIS config written: ${NSIS_FILE}"

    # Copy binary to NSIS directory for makensis
    if [[ -f "${BUILD_DIR}/rexplayer.exe" ]]; then
        cp "${BUILD_DIR}/rexplayer.exe" "${NSIS_DIR}/${APP_NAME}.exe"
        find "${BUILD_DIR}" -name "*.dll" -exec cp {} "${NSIS_DIR}/" \; 2>/dev/null || true
    fi

    # Run makensis if available
    if command -v makensis &>/dev/null; then
        echo "[Windows] Running makensis..."
        (cd "${NSIS_DIR}" && makensis installer.nsi)
        mv "${NSIS_DIR}/${APP_NAME}-${VERSION}-Setup.exe" "${OUTPUT_DIR}/"
        echo "[Windows] Installer created: ${OUTPUT_DIR}/${APP_NAME}-${VERSION}-Setup.exe"
    else
        echo "[Windows] makensis not found — NSIS script generated but installer not built."
        echo "  Install NSIS and run: makensis ${NSIS_FILE}"
    fi
}

# ══════════════════════════════════════════════════════════════════════
# Dispatch
# ══════════════════════════════════════════════════════════════════════
case "${PLATFORM}" in
    linux)   package_linux   ;;
    macos)   package_macos   ;;
    windows) package_windows ;;
esac

echo ""
echo "Packaging complete. Output: ${OUTPUT_DIR}/"
ls -lh "${OUTPUT_DIR}/" 2>/dev/null || true
