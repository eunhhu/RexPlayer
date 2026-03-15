#!/usr/bin/env bash
# fetch_frida.sh — Download the latest Frida server binary from GitHub Releases
#
# Detects host architecture, queries the GitHub API for the latest (or
# specified) Frida release, downloads the frida-server binary, and
# extracts it to an output directory.
#
# Usage:
#   ./scripts/fetch_frida.sh [--version VERSION] [--output DIR] [--arch ARCH] [--platform PLATFORM]
#
# Examples:
#   ./scripts/fetch_frida.sh                             # latest, auto-detect arch
#   ./scripts/fetch_frida.sh --version 16.5.6            # specific version
#   ./scripts/fetch_frida.sh --output ./tools --arch arm64 --platform android

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────
FRIDA_VERSION=""           # empty = latest
OUTPUT_DIR="./third_party/frida"
FORCE_ARCH=""
FORCE_PLATFORM=""
GITHUB_API="https://api.github.com/repos/frida/frida/releases"
KEEP_ARCHIVE=false

# ── Argument parsing ─────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version|-v)    FRIDA_VERSION="$2";  shift 2 ;;
        --output|-o)     OUTPUT_DIR="$2";     shift 2 ;;
        --arch|-a)       FORCE_ARCH="$2";     shift 2 ;;
        --platform|-p)   FORCE_PLATFORM="$2"; shift 2 ;;
        --keep-archive)  KEEP_ARCHIVE=true;   shift   ;;
        --help|-h)
            echo "Usage: $0 [--version VER] [--output DIR] [--arch ARCH] [--platform PLATFORM] [--keep-archive]"
            exit 0 ;;
        *)
            echo "Error: unknown argument '$1'" >&2; exit 1 ;;
    esac
done

# ── Detect host architecture ─────────────────────────────────────────
detect_arch() {
    if [[ -n "${FORCE_ARCH}" ]]; then
        echo "${FORCE_ARCH}"
        return
    fi

    local machine
    machine="$(uname -m)"
    case "${machine}" in
        x86_64|amd64)   echo "x86_64"  ;;
        aarch64|arm64)  echo "arm64"   ;;
        armv7l|armhf)   echo "arm"     ;;
        i686|i386)      echo "x86"     ;;
        *)
            echo "Error: unsupported architecture '${machine}'" >&2
            exit 1
            ;;
    esac
}

# ── Detect host platform ─────────────────────────────────────────────
detect_platform() {
    if [[ -n "${FORCE_PLATFORM}" ]]; then
        echo "${FORCE_PLATFORM}"
        return
    fi

    local os
    os="$(uname -s)"
    case "${os}" in
        Linux)   echo "linux"   ;;
        Darwin)  echo "macos"   ;;
        MINGW*|MSYS*|CYGWIN*) echo "windows" ;;
        *)
            echo "Error: unsupported OS '${os}'" >&2
            exit 1
            ;;
    esac
}

ARCH="$(detect_arch)"
PLATFORM="$(detect_platform)"

echo "Frida server downloader"
echo "  Architecture : ${ARCH}"
echo "  Platform     : ${PLATFORM}"

# ── Check dependencies ───────────────────────────────────────────────
for cmd in curl jq xz; do
    if ! command -v "${cmd}" &>/dev/null; then
        echo "Error: required command '${cmd}' not found. Please install it." >&2
        exit 1
    fi
done

# ── Resolve version ──────────────────────────────────────────────────
if [[ -z "${FRIDA_VERSION}" ]]; then
    echo "  Querying latest release from GitHub..."
    FRIDA_VERSION="$(curl -fsSL "${GITHUB_API}/latest" | jq -r '.tag_name')"
    if [[ -z "${FRIDA_VERSION}" || "${FRIDA_VERSION}" == "null" ]]; then
        echo "Error: could not determine latest Frida version." >&2
        exit 1
    fi
fi

echo "  Version      : ${FRIDA_VERSION}"

# ── Check if already downloaded ──────────────────────────────────────
VERSION_FILE="${OUTPUT_DIR}/.frida-version"
if [[ -f "${VERSION_FILE}" ]]; then
    EXISTING="$(cat "${VERSION_FILE}")"
    if [[ "${EXISTING}" == "${FRIDA_VERSION}" ]]; then
        echo "  Already at version ${FRIDA_VERSION} — skipping download."
        exit 0
    fi
    echo "  Upgrading from ${EXISTING} to ${FRIDA_VERSION}..."
fi

# ── Build the asset filename ─────────────────────────────────────────
# Frida release assets follow the pattern:
#   frida-server-<version>-<platform>-<arch>.xz
ASSET_NAME="frida-server-${FRIDA_VERSION}-${PLATFORM}-${ARCH}.xz"
DOWNLOAD_URL="https://github.com/frida/frida/releases/download/${FRIDA_VERSION}/${ASSET_NAME}"

echo "  Asset        : ${ASSET_NAME}"
echo "  URL          : ${DOWNLOAD_URL}"

# ── Download ─────────────────────────────────────────────────────────
mkdir -p "${OUTPUT_DIR}"
ARCHIVE_PATH="${OUTPUT_DIR}/${ASSET_NAME}"

echo "  Downloading..."
if ! curl -fSL --progress-bar -o "${ARCHIVE_PATH}" "${DOWNLOAD_URL}"; then
    echo "Error: download failed. Check that version '${FRIDA_VERSION}' exists" >&2
    echo "  and has an asset for ${PLATFORM}-${ARCH}." >&2
    rm -f "${ARCHIVE_PATH}"
    exit 1
fi

# ── Extract ──────────────────────────────────────────────────────────
BINARY_NAME="frida-server"
BINARY_PATH="${OUTPUT_DIR}/${BINARY_NAME}"

echo "  Extracting..."
xz -d -f -k "${ARCHIVE_PATH}"
# xz -d produces a file without the .xz extension
DECOMPRESSED="${ARCHIVE_PATH%.xz}"
mv "${DECOMPRESSED}" "${BINARY_PATH}"
chmod +x "${BINARY_PATH}"

# Clean up archive unless --keep-archive was passed
if [[ "${KEEP_ARCHIVE}" != true ]]; then
    rm -f "${ARCHIVE_PATH}"
fi

# ── Write version marker ─────────────────────────────────────────────
echo "${FRIDA_VERSION}" > "${VERSION_FILE}"

# ── Verify ───────────────────────────────────────────────────────────
FILE_SIZE="$(du -sh "${BINARY_PATH}" | cut -f1)"
echo ""
echo "Done. Frida server ${FRIDA_VERSION} (${PLATFORM}/${ARCH})"
echo "  Binary : ${BINARY_PATH} (${FILE_SIZE})"
