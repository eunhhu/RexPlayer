#!/usr/bin/env python3
"""Validate the committed RexPlayer runtime-proof evidence without privileged access."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent
EVIDENCE = ROOT / "evidence"

EXPECTED_EVENTS = [
    ("EV_KEY", "BTN_TOUCH", "DOWN"),
    ("EV_ABS", "ABS_MT_TRACKING_ID", "0000002a"),
    ("EV_ABS", "ABS_MT_POSITION_X", "00000064"),
    ("EV_ABS", "ABS_MT_POSITION_Y", "000000c8"),
    ("EV_ABS", "ABS_X", "00000064"),
    ("EV_ABS", "ABS_Y", "000000c8"),
    ("EV_SYN", "SYN_REPORT", "00000000"),
    ("EV_ABS", "ABS_MT_POSITION_X", "00000190"),
    ("EV_ABS", "ABS_MT_POSITION_Y", "000001f4"),
    ("EV_ABS", "ABS_X", "00000190"),
    ("EV_ABS", "ABS_Y", "000001f4"),
    ("EV_SYN", "SYN_REPORT", "00000000"),
    ("EV_ABS", "ABS_MT_TRACKING_ID", "ffffffff"),
    ("EV_KEY", "BTN_TOUCH", "UP"),
    ("EV_SYN", "SYN_REPORT", "00000000"),
]

EXPECTED_NUMERIC_EVENTS = [
    (1, 330, 1),
    (3, 57, 42),
    (3, 53, 100),
    (3, 54, 200),
    (3, 0, 100),
    (3, 1, 200),
    (0, 0, 0),
    (3, 53, 400),
    (3, 54, 500),
    (3, 0, 400),
    (3, 1, 500),
    (0, 0, 0),
    (3, 57, -1),
    (1, 330, 0),
    (0, 0, 0),
]

REGULAR_CHECKS = {
    "qemu_flag", "hardware", "boot_hardware", "product_model", "product_device",
    "product_name", "manufacturer", "brand", "fingerprint", "build_type",
    "build_tags", "ro_debuggable", "ro_secure", "adb_secure", "verified_boot",
    "flash_locked", "vbmeta_state", "selinux_enforcing", "proc_cmdline",
    "proc_mountinfo", "proc_cgroup", "proc_cpuinfo", "qemu_devices",
    "goldfish_sysfs", "su_binary", "root_artifacts", "frida_process",
    "frida_ports", "frida_maps", "adb_enabled", "graphics_props", "telephony",
    "serial_number", "adb_shell_privilege",
}
INFO_CHECKS = {"kernel", "page_size", "sdk", "release", "security_patch", "density", "size"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def parse_key_values(path: Path) -> dict[str, str]:
    require(path.is_file(), f"missing evidence file: {path.name}")
    result: dict[str, str] = {}
    for line in path.read_text().splitlines():
        key, separator, value = line.partition("=")
        require(bool(separator) and key not in result, f"invalid key/value row in {path}: {line!r}")
        result[key] = value
    return result


def validate_matrix(path: Path) -> None:
    lines = path.read_text().splitlines()
    require(bool(lines) and lines[0] == "VERDICT\tCHECK\tEVIDENCE", "invalid matrix header")
    seen: set[str] = set()
    counts = {key: 0 for key in ("PASS", "FAIL", "SKIP", "INFO")}
    for number, line in enumerate(lines[1:], start=2):
        parts = line.split("\t")
        require(len(parts) == 3 and bool(parts[2]), f"malformed matrix row {number}")
        verdict, check, _ = parts
        require(check not in seen, f"duplicate matrix check: {check}")
        seen.add(check)
        if check in REGULAR_CHECKS:
            require(verdict in {"PASS", "FAIL", "SKIP"}, f"invalid verdict for {check}: {verdict}")
        elif check in INFO_CHECKS:
            require(verdict == "INFO", f"invalid info verdict for {check}: {verdict}")
        else:
            raise SystemExit(f"unknown matrix check: {check}")
        counts[verdict] += 1
    require(seen == REGULAR_CHECKS | INFO_CHECKS, "matrix check set mismatch")
    require(counts == {"PASS": 8, "FAIL": 23, "SKIP": 3, "INFO": 7}, f"unexpected matrix counts: {counts}")


def validate_getevent(path: Path) -> None:
    pattern = re.compile(r"\]\s+(EV_[A-Z]+)\s+(\S+)\s+(\S+)\s*$")
    observed = []
    for line in path.read_text().splitlines():
        match = pattern.search(line)
        if match:
            observed.append(match.groups())
    require(observed == EXPECTED_EVENTS, f"Android event sequence mismatch: {observed!r}")


def validate_numeric_log(path: Path) -> None:
    pattern = re.compile(r"^EVENT type=(\d+) code=(\d+) value=(-?\d+)$")
    observed = []
    for line in path.read_text().splitlines():
        match = pattern.fullmatch(line)
        if match:
            observed.append(tuple(map(int, match.groups())))
    require(observed == EXPECTED_NUMERIC_EVENTS, f"numeric event sequence mismatch in {path.name}: {observed!r}")


def validate_inputreader(path: Path) -> None:
    text = path.read_text()
    reader = text.split("Input Reader State", 1)
    require(len(reader) == 2, "Input Reader section missing")
    match = re.search(
        r"\n  Device -?\d+: RexPlayer Virtual Multi-Touch Proof\n(.*?)(?=\n  Device -?\d+:|\n\S|\Z)",
        reader[1],
        re.S,
    )
    if match is None:
        raise SystemExit("named proof device missing from InputReader")
    block = match.group(1)
    require("Sources: TOUCHSCREEN" in block, "TOUCHSCREEN source missing")
    require("DeviceType: TOUCH_SCREEN" in block, "TOUCH_SCREEN classification missing")


def validate_windows_wsl() -> None:
    standard = parse_key_values(EVIDENCE / "windows-wsl-standard-preflight.txt")
    expected_standard = {
        "ARCH": "x86_64",
        "PAGE_SIZE": "4096",
        "VIRTUALIZATION": "wsl",
        "CONFIG_ANDROID_BINDER_IPC": "n",
        "CONFIG_INPUT_UINPUT": "m",
        "DEVICE_uinput": "PRESENT",
        "DEVICE_dxg": "PRESENT",
        "BINDERFS_MOUNTABLE": "NO",
    }
    for key, value in expected_standard.items():
        require(standard.get(key) == value, f"unexpected standard WSL {key}={standard.get(key)!r}")

    custom = parse_key_values(EVIDENCE / "windows-wsl-custom-preflight.txt")
    expected_custom = {
        "ARCH": "x86_64",
        "PAGE_SIZE": "4096",
        "VIRTUALIZATION": "wsl",
        "CONFIG_ANDROID_BINDER_IPC": "y",
        "CONFIG_ANDROID_BINDERFS": "y",
        "CONFIG_INPUT_UINPUT": "y",
        "CONFIG_INPUT_EVDEV": "y",
        "DEVICE_uinput": "PRESENT",
        "DEVICE_dxg": "PRESENT",
        "BINDERFS_MOUNTABLE": "YES",
    }
    for key, value in expected_custom.items():
        require(custom.get(key) == value, f"unexpected custom WSL {key}={custom.get(key)!r}")
    require("rexplayer-wsl" in custom.get("KERNEL", ""), "custom WSL kernel marker missing")


def validate_android_summary(path: Path) -> None:
    summary = parse_key_values(path)
    expected = {
        "PRODUCER_EXIT": "0",
        "PRODUCER_PASS": "YES",
        "GETEVENT_EVENT_LINES": "15",
        "GETEVENT_SEQUENCE": "YES",
        "INPUTREADER_REGISTERED": "YES",
        "TOUCHSCREEN_SOURCE": "YES",
        "TOUCHSCREEN_DEVICE_TYPE": "YES",
        "RESULT": "PASS",
    }
    for key, value in expected.items():
        require(summary.get(key) == value, f"unexpected {path.name} {key}={summary.get(key)!r}")


def validate_windows_wsl_android() -> None:
    windows_native = parse_key_values(EVIDENCE / "windows-native-validation.txt")
    require(windows_native.get("PLATFORM") == "Windows PowerShell", "unexpected Windows native platform")
    require(windows_native.get("POWERSHELL_SHA256") == "PASS", "Windows native SHA-256 validation missing")
    require(
        windows_native.get("POWERSHELL_TRAVERSAL_REJECTION") == "PASS",
        "Windows native manifest traversal rejection missing",
    )
    require(
        windows_native.get("POWERSHELL_REPARSE_REJECTION") == "PASS",
        "Windows native manifest reparse-point rejection missing",
    )

    runtime = parse_key_values(EVIDENCE / "windows-wsl-android-runtime.txt")
    expected = {
        "KERNEL": "6.18.40.1-rexplayer-wsl+",
        "ARCH": "x86_64",
        "PAGE_SIZE": "4096",
        "VIRTUALIZATION": "wsl",
        "DOCKER_SERVER": "29.1.3",
        "IMAGE_ARCH": "amd64",
        "BINDER_HOST_OPEN": "binder,hwbinder,vndbinder",
        "BINDER_CONTAINER_BIND_OPEN": "binder,hwbinder,vndbinder",
        "BINDER_MAPPING": "bind-mount",
        "ANDROID_BOOT": "PASS",
        "ADB_STATE": "device",
        "ANDROID_RELEASE": "14",
        "ANDROID_SDK": "34",
        "ANDROID_ABI": "x86_64",
        "ANDROID_INPUT": "PASS",
        "MATRIX_PASS": "8",
        "MATRIX_FAIL": "23",
        "MATRIX_SKIP": "3",
        "DETECTABLE": "YES",
        "POST_RUN_CLEANUP": "PASS",
        "BINDER_MOUNT_ID_GUARD": "PASS",
        "RETAIN_BYPASS_DISABLED": "PASS",
        "BASH_ENV_CLEANUP_GUARD": "PASS",
        "FINAL_PASS_AFTER_CLEANUP": "PASS",
        "WINDOWS_WSL_ANDROID_E2E": "PASS",
    }
    for key, value in expected.items():
        require(runtime.get(key) == value, f"unexpected Windows WSL Android {key}={runtime.get(key)!r}")
    require(
        runtime.get("IMAGE")
        == "redroid/redroid@sha256:11d58a64bfbde2253d1cce81bff409ff58174980222d1bada232d9ef59181191",
        "unexpected Windows WSL Android image",
    )
    validate_numeric_log(EVIDENCE / "windows-wsl-android-input-producer.log")
    validate_android_summary(EVIDENCE / "windows-wsl-android-input-summary.txt")
    validate_getevent(EVIDENCE / "windows-wsl-android-getevent.log")
    validate_inputreader(EVIDENCE / "windows-wsl-android-inputreader.txt")
    validate_matrix(EVIDENCE / "windows-wsl-detection-matrix-result.tsv")


def main() -> None:
    for name in (
        "host-input-result.log",
        "rust-host-input-result.log",
        "android-input-producer.log",
        "windows-wsl-uinput-result.log",
    ):
        path = EVIDENCE / name
        text = path.read_text()
        validate_numeric_log(path)
        require("SUMMARY events=15 expected=15 syn_reports=3 sequence=1" in text, f"exact summary missing: {name}")
        require("RESULT PASS" in text, f"PASS missing: {name}")

    validate_android_summary(EVIDENCE / "android-input-summary.txt")

    environment = parse_key_values(EVIDENCE / "environment.txt")
    require(environment.get("ARCH") == "aarch64", "unexpected proof architecture")
    require(environment.get("PAGE_SIZE") == "4096", "unexpected proof page size")
    require("binder_linux" in environment.get("BINDER_MODULE", ""), "binder_linux evidence missing")

    validate_getevent(EVIDENCE / "android-getevent.log")
    validate_inputreader(EVIDENCE / "android-inputreader.txt")
    validate_matrix(EVIDENCE / "detection-matrix-result.tsv")
    validate_windows_wsl()
    validate_windows_wsl_android()
    print("EVIDENCE_VALIDATION=PASS")


if __name__ == "__main__":
    main()
