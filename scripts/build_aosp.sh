#!/usr/bin/env bash
# build_aosp.sh — Build AOSP with the RexPlayer device tree
#
# This script automates the AOSP build for the RexPlayer virtual device.
# It sources the AOSP environment, selects the lunch target, and runs
# the build with the correct configuration.
#
# Prerequisites:
#   - AOSP source tree checked out (repo sync completed)
#   - RexPlayer device tree placed in device/rex/rexplayer/
#   - JDK, build-essential, and other AOSP deps installed
#
# Usage:
#   ./scripts/build_aosp.sh [--jobs N] [--variant userdebug|eng|user]

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# AOSP source tree — override with AOSP_ROOT env var if needed
AOSP_ROOT="${AOSP_ROOT:-${HOME}/aosp}"

# Device configuration
DEVICE_VENDOR="rex"
DEVICE_NAME="rexplayer"
LUNCH_TARGET="${DEVICE_NAME}"

# Build settings
BUILD_VARIANT="userdebug"       # user | userdebug | eng
BUILD_JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
BUILD_MODULE=""                  # empty = full build; set to e.g. "SystemUI" for module build

# Output
OUT_DIR="${AOSP_ROOT}/out"

# ── Argument parsing ─────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs|-j)
            BUILD_JOBS="$2"; shift 2 ;;
        --variant|-v)
            BUILD_VARIANT="$2"; shift 2 ;;
        --module|-m)
            BUILD_MODULE="$2"; shift 2 ;;
        --aosp-root)
            AOSP_ROOT="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--jobs N] [--variant userdebug|eng|user] [--module NAME] [--aosp-root PATH]"
            exit 0 ;;
        *)
            echo "Error: unknown argument '$1'" >&2; exit 1 ;;
    esac
done

# ── Validation ────────────────────────────────────────────────────────
if [[ ! -d "${AOSP_ROOT}/build/envsetup.sh" ]] && [[ ! -f "${AOSP_ROOT}/build/envsetup.sh" ]]; then
    echo "Error: AOSP source tree not found at ${AOSP_ROOT}" >&2
    echo "  Set AOSP_ROOT or use --aosp-root to specify the path." >&2
    exit 1
fi

DEVICE_TREE="${AOSP_ROOT}/device/${DEVICE_VENDOR}/${DEVICE_NAME}"
if [[ ! -d "${DEVICE_TREE}" ]]; then
    echo "Error: RexPlayer device tree not found at ${DEVICE_TREE}" >&2
    echo "  Copy the device tree from ${PROJECT_ROOT}/android/device/ first." >&2
    exit 1
fi

echo "══════════════════════════════════════════════════════════════"
echo "  RexPlayer AOSP Build"
echo "══════════════════════════════════════════════════════════════"
echo "  AOSP root  : ${AOSP_ROOT}"
echo "  Device     : ${DEVICE_VENDOR}/${DEVICE_NAME}"
echo "  Variant    : ${BUILD_VARIANT}"
echo "  Jobs       : ${BUILD_JOBS}"
echo "  Out dir    : ${OUT_DIR}"
echo "══════════════════════════════════════════════════════════════"

# ── Step 1: Source the AOSP build environment ─────────────────────────
# envsetup.sh defines lunch, m, mm, mmm, and other build helpers.
echo "[1/4] Sourcing build environment..."
# shellcheck disable=SC1091
source "${AOSP_ROOT}/build/envsetup.sh"

# ── Step 2: Select the build target via lunch ─────────────────────────
# lunch sets TARGET_PRODUCT, TARGET_BUILD_VARIANT, and configures the
# build system for our device.
echo "[2/4] Selecting lunch target: ${LUNCH_TARGET}-${BUILD_VARIANT}..."
lunch "${LUNCH_TARGET}-${BUILD_VARIANT}"

# ── Step 3: Build ─────────────────────────────────────────────────────
# Full build produces system.img, vendor.img, etc. in out/target/product/<device>/
# Module build produces just the specified module APK/binary.
if [[ -n "${BUILD_MODULE}" ]]; then
    echo "[3/4] Building module: ${BUILD_MODULE} (-j${BUILD_JOBS})..."
    m "${BUILD_MODULE}" -j"${BUILD_JOBS}"
else
    echo "[3/4] Starting full build (-j${BUILD_JOBS})..."
    m -j"${BUILD_JOBS}"
fi

# ── Step 4: Verify output ────────────────────────────────────────────
PRODUCT_OUT="${OUT_DIR}/target/product/${DEVICE_NAME}"
echo "[4/4] Checking build artifacts..."

EXPECTED_IMAGES=("system.img" "vendor.img" "boot.img")
MISSING=0
for img in "${EXPECTED_IMAGES[@]}"; do
    if [[ -f "${PRODUCT_OUT}/${img}" ]]; then
        SIZE=$(du -sh "${PRODUCT_OUT}/${img}" | cut -f1)
        echo "  ✓ ${img} (${SIZE})"
    else
        echo "  ✗ ${img} — MISSING" >&2
        MISSING=$((MISSING + 1))
    fi
done

if [[ ${MISSING} -gt 0 ]]; then
    echo "Error: ${MISSING} expected image(s) not found." >&2
    exit 1
fi

echo ""
echo "Build complete. Images are in: ${PRODUCT_OUT}/"
