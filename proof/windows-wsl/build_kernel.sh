#!/usr/bin/env bash
set -euo pipefail

SOURCE_REPOSITORY=${SOURCE_REPOSITORY:-https://github.com/microsoft/WSL2-Linux-Kernel.git}
SOURCE_BRANCH=${SOURCE_BRANCH:-linux-msft-wsl-6.18.y}
SOURCE_COMMIT=${SOURCE_COMMIT:-14794180686c2fb6307fbe359c359bec765249f3}
WORK_DIR=${1:-"$PWD/.kernel-work"}
OUTPUT_DIR=${2:-"$PWD/out"}
JOBS=${JOBS:-3}
SOURCE_DIR="$WORK_DIR/WSL2-Linux-Kernel"

required=(bc bison flex gcc git make openssl pahole python3 sha256sum)
for command_name in "${required[@]}"; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'ERROR missing build dependency: %s\n' "$command_name" >&2
    exit 2
  fi
done

mkdir -p "$WORK_DIR" "$OUTPUT_DIR"
if [ ! -d "$SOURCE_DIR/.git" ]; then
  git clone --depth 1 --filter=blob:none --no-checkout --branch "$SOURCE_BRANCH" \
    "$SOURCE_REPOSITORY" "$SOURCE_DIR"
fi
git -C "$SOURCE_DIR" fetch --depth 1 origin "$SOURCE_COMMIT"
git -C "$SOURCE_DIR" checkout --detach "$SOURCE_COMMIT"

cd "$SOURCE_DIR"
make mrproper
cp Microsoft/config-wsl .config
scripts/config --enable ANDROID_BINDER_IPC
scripts/config --enable ANDROID_BINDERFS
scripts/config --set-str ANDROID_BINDER_DEVICES 'binder,hwbinder,vndbinder'
scripts/config --enable INPUT_UINPUT
scripts/config --enable INPUT_EVDEV
scripts/config --set-str LOCALVERSION '-rexplayer-wsl'
scripts/config --disable DEBUG_INFO
scripts/config --disable DEBUG_INFO_BTF
scripts/config --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT
make olddefconfig

python3 - <<'PY'
from pathlib import Path

expected = {
    "CONFIG_ANDROID_BINDER_IPC": "y",
    "CONFIG_ANDROID_BINDERFS": "y",
    "CONFIG_ANDROID_BINDER_DEVICES": '"binder,hwbinder,vndbinder"',
    "CONFIG_INPUT_UINPUT": "y",
    "CONFIG_INPUT_EVDEV": "y",
    "CONFIG_LOCALVERSION": '"-rexplayer-wsl"',
}
values = {}
for line in Path(".config").read_text().splitlines():
    if line.startswith("CONFIG_") and "=" in line:
        key, value = line.split("=", 1)
        values[key] = value
for key, wanted in expected.items():
    actual = values.get(key)
    print(f"{key}={actual}")
    if actual != wanted:
        raise SystemExit(f"{key}: expected {wanted}, got {actual}")
PY

nice -n 10 make -j"$JOBS" bzImage
install -m 0644 arch/x86/boot/bzImage "$OUTPUT_DIR/bzImage-rexplayer-wsl"
install -m 0644 .config "$OUTPUT_DIR/config-rexplayer-wsl"
printf '%s\n' "$SOURCE_COMMIT" > "$OUTPUT_DIR/kernel-source-commit.txt"
sha256sum \
  "$OUTPUT_DIR/bzImage-rexplayer-wsl" \
  "$OUTPUT_DIR/config-rexplayer-wsl" \
  "$OUTPUT_DIR/kernel-source-commit.txt" > "$OUTPUT_DIR/SHA256SUMS.txt"
printf 'KERNEL_RELEASE=%s\n' "$(make -s kernelrelease)"
printf 'BUILD_RESULT=PASS\n'
