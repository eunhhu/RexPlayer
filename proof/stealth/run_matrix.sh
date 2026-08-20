#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
SERIAL="${1:-127.0.0.1:5555}"

timeout 10 adb connect "$SERIAL" >/dev/null 2>&1 || true
state=''
for _ in $(seq 1 120); do
  state="$(adb -s "$SERIAL" get-state 2>/dev/null || true)"
  [ "$state" = device ] && break
  sleep 1
done
if [ "$state" != device ]; then
  printf 'ADB device did not become ready: %s\n' "$SERIAL" >&2
  exit 2
fi

boot=''
for _ in $(seq 1 120); do
  boot="$(timeout 5 adb -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' || true)"
  [ "$boot" = 1 ] && break
  sleep 1
done
if [ "$boot" != 1 ]; then
  printf 'Android did not complete boot\n' >&2
  exit 2
fi

timeout 60 adb -s "$SERIAL" shell sh -s < "$ROOT/android_detection_matrix.sh" | tr -d '\r' | tee "$ROOT/result.tsv"
timeout 30 adb -s "$SERIAL" shell getprop | tr -d '\r' > "$ROOT/getprop.txt"
timeout 30 adb -s "$SERIAL" shell dumpsys input | tr -d '\r' > "$ROOT/dumpsys-input.txt"
timeout 30 adb -s "$SERIAL" shell dumpsys SurfaceFlinger | tr -d '\r' > "$ROOT/dumpsys-surfaceflinger.txt"
timeout 30 adb -s "$SERIAL" shell dumpsys sensorservice | tr -d '\r' > "$ROOT/dumpsys-sensorservice.txt"
timeout 30 adb -s "$SERIAL" shell dumpsys media.drm | tr -d '\r' > "$ROOT/dumpsys-media-drm.txt" || true

python3 - "$ROOT/result.tsv" <<'PY'
from pathlib import Path
import sys

regular = {
    "qemu_flag", "hardware", "boot_hardware", "product_model", "product_device",
    "product_name", "manufacturer", "brand", "fingerprint", "build_type",
    "build_tags", "ro_debuggable", "ro_secure", "adb_secure", "verified_boot",
    "flash_locked", "vbmeta_state", "selinux_enforcing", "proc_cmdline",
    "proc_mountinfo", "proc_cgroup", "proc_cpuinfo", "qemu_devices",
    "goldfish_sysfs", "su_binary", "root_artifacts", "frida_process",
    "frida_ports", "frida_maps", "adb_enabled", "graphics_props", "telephony",
    "serial_number", "adb_shell_privilege",
}
info_checks = {"kernel", "page_size", "sdk", "release", "security_patch", "density", "size"}
path = Path(sys.argv[1])
lines = path.read_text().splitlines()
if not lines or lines[0] != "VERDICT\tCHECK\tEVIDENCE":
    raise SystemExit("invalid matrix header")

rows = []
seen = set()
for number, line in enumerate(lines[1:], start=2):
    parts = line.split("\t")
    if len(parts) != 3:
        raise SystemExit(f"malformed matrix row {number}")
    verdict, check, evidence = parts
    if not evidence:
        raise SystemExit(f"empty evidence at row {number}")
    if check in seen:
        raise SystemExit(f"duplicate matrix check: {check}")
    seen.add(check)
    if check in regular and verdict not in {"PASS", "FAIL", "SKIP"}:
        raise SystemExit(f"invalid verdict for {check}: {verdict}")
    if check in info_checks and verdict != "INFO":
        raise SystemExit(f"invalid info verdict for {check}: {verdict}")
    if check not in regular and check not in info_checks:
        raise SystemExit(f"unknown matrix check: {check}")
    rows.append(parts)

expected = regular | info_checks
missing = sorted(expected - seen)
extra = sorted(seen - expected)
if missing or extra:
    raise SystemExit(f"matrix check set mismatch missing={missing} extra={extra}")

counts = {key: sum(row[0] == key for row in rows) for key in ("PASS", "FAIL", "SKIP", "INFO")}
print(
    f"MATRIX_SUMMARY pass={counts['PASS']} fail={counts['FAIL']} "
    f"skip={counts['SKIP']} info={counts['INFO']}"
)
if counts["FAIL"]:
    print("DETECTABLE=YES")
    raise SystemExit(3)
if counts["SKIP"]:
    print("DETECTABILITY=INCONCLUSIVE")
    raise SystemExit(4)
print("DETECTABLE=NO_FOR_TESTED_VECTORS")
PY
