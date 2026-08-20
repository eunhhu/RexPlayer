#!/usr/bin/env bash
set -euo pipefail

config_value() {
  local key="$1"
  python3 - "$key" <<'PY'
from pathlib import Path
import gzip
import os
import sys
key = sys.argv[1]
paths = [Path("/proc/config.gz"), Path("/boot") / f"config-{os.uname().release}"]
text = ""
for path in paths:
    if not path.exists():
        continue
    text = gzip.open(path, "rt").read() if path.suffix == ".gz" else path.read_text()
    break
value = "UNKNOWN"
for line in text.splitlines():
    if line.startswith(key + "="):
        value = line.split("=", 1)[1]
        break
    if line == f"# {key} is not set":
        value = "n"
        break
print(value)
PY
}

printf 'KERNEL=%s\n' "$(uname -r)"
printf 'ARCH=%s\n' "$(uname -m)"
printf 'PAGE_SIZE=%s\n' "$(getconf PAGE_SIZE)"
printf 'VIRTUALIZATION=%s\n' "$(systemd-detect-virt || true)"
printf 'CONFIG_ANDROID_BINDER_IPC=%s\n' "$(config_value CONFIG_ANDROID_BINDER_IPC)"
printf 'CONFIG_ANDROID_BINDERFS=%s\n' "$(config_value CONFIG_ANDROID_BINDERFS)"
printf 'CONFIG_INPUT_UINPUT=%s\n' "$(config_value CONFIG_INPUT_UINPUT)"
printf 'CONFIG_INPUT_EVDEV=%s\n' "$(config_value CONFIG_INPUT_EVDEV)"
for path in /dev/uinput /dev/dxg /dev/binderfs /dev/binder /dev/hwbinder /dev/vndbinder; do
  if [ -e "$path" ]; then
    printf 'DEVICE_%s=PRESENT\n' "${path#/dev/}"
  else
    printf 'DEVICE_%s=MISSING\n' "${path#/dev/}"
  fi
done
if command -v nvidia-smi >/dev/null 2>&1; then
  printf 'GPU='
  nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader | python3 -c 'import sys; print(sys.stdin.read().strip())'
else
  printf 'GPU=UNAVAILABLE\n'
fi
probe="$(mktemp -d)"
cleanup() {
  mountpoint -q "$probe" && umount "$probe" || true
  rmdir "$probe" 2>/dev/null || true
}
trap cleanup EXIT
if mount -t binder binder "$probe" 2>/dev/null; then
  printf 'BINDERFS_MOUNTABLE=YES\n'
else
  printf 'BINDERFS_MOUNTABLE=NO\n'
fi
