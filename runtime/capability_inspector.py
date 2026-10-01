#!/usr/bin/env python3
"""Read-only RexPlayer host inventory. This is not a runtime readiness test.

No processes, devices, containers, distributions, services, mounts, or network
connections are opened or started. Reports contain no usernames, hostnames,
configuration values, full environment, or discovered executable paths.
"""

from __future__ import annotations

import argparse
import configparser
import gzip
import json
import os
import platform
import shutil
import stat
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SCHEMA_VERSION = "1.0"
INSPECTOR_VERSION = "0.1.0"
MAX_SOURCE_BYTES = 2 * 1024 * 1024
KERNEL_FEATURES = {
    "CONFIG_ANDROID_BINDER_IPC": "Android Binder IPC",
    "CONFIG_ANDROID_BINDERFS": "Binder filesystem",
    "CONFIG_INPUT_UINPUT": "Userspace input devices",
    "CONFIG_INPUT_EVDEV": "Event input interface",
    "CONFIG_NAMESPACES": "Namespaces",
    "CONFIG_UTS_NS": "UTS namespaces",
    "CONFIG_IPC_NS": "IPC namespaces",
    "CONFIG_PID_NS": "PID namespaces",
    "CONFIG_NET_NS": "Network namespaces",
    "CONFIG_CGROUPS": "Control groups",
}
RUNTIME_GATES = (
    "android_boot", "android_app_input", "graphics_rendering", "audio",
    "lifecycle_recovery", "instance_isolation", "management_security",
    "performance", "install_update_rollback",
)
STATUSES = frozenset({"available", "unavailable", "unknown", "not_applicable"})


@dataclass(frozen=True)
class Observation:
    status: str
    value: Any = None
    reason: str = ""


def failure(exc: OSError) -> Observation:
    if isinstance(exc, (FileNotFoundError, NotADirectoryError)):
        return Observation("unavailable", reason="not_found")
    if isinstance(exc, PermissionError):
        return Observation("unknown", reason="permission_denied")
    return Observation("unknown", reason="os_error")


class HostProbe:
    """Small read-only OS boundary, replaceable with fixtures in tests."""

    def metadata(self) -> dict[str, str]:
        return {
            "os": platform.system(),
            "architecture": platform.machine(),
            "kernel_release": platform.release(),
            "os_version": platform.version(),
        }

    def read(self, path: str, *, compressed: bool = False) -> Observation:
        # Nonblocking open plus fstat avoids accidentally reading a FIFO/device
        # substituted for a config file. Bounded decompression prevents a huge
        # or malformed config from consuming unbounded memory.
        fd = None
        try:
            # Reject known special files before opening; reject symlinks to avoid
            # traversing a substituted config into unrelated private content.
            if not stat.S_ISREG(os.lstat(path).st_mode):
                return Observation("unknown", reason="not_regular_file")
            fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
                         | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_BINARY", 0))
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                return Observation("unknown", reason="not_regular_file")
            with os.fdopen(fd, "rb") as stream:
                fd = None
                if compressed:
                    with gzip.GzipFile(fileobj=stream, mode="rb") as decoded:
                        data = decoded.read(MAX_SOURCE_BYTES + 1)
                else:
                    data = stream.read(MAX_SOURCE_BYTES + 1)
            if len(data) > MAX_SOURCE_BYTES:
                return Observation("unknown", reason="source_too_large")
            return Observation("available", data.decode("utf-8-sig"))
        except (UnicodeError, EOFError, gzip.BadGzipFile, zlib.error):
            return Observation("unknown", reason="invalid_content")
        except OSError as exc:
            return failure(exc)
        finally:
            if fd is not None:
                os.close(fd)

    def path(self, path: str) -> Observation:
        try:
            mode = os.stat(path).st_mode
        except OSError as exc:
            return failure(exc)
        kind = ("character_device" if stat.S_ISCHR(mode) else
                "directory" if stat.S_ISDIR(mode) else
                "regular_file" if stat.S_ISREG(mode) else "other")
        return Observation("available", kind)

    def access(self, path: str) -> Observation:
        try:
            kwargs = {"effective_ids": True} if os.access in os.supports_effective_ids else {}
            return Observation("available", os.access(path, os.R_OK | os.W_OK, **kwargs))
        except (NotImplementedError, TypeError):
            return Observation("unknown", reason="access_check_unsupported")
        except OSError as exc:
            return failure(exc)

    def tool(self, name: str) -> Observation:
        try:
            found = shutil.which(name) is not None
        except OSError as exc:
            return failure(exc)
        return Observation("available" if found else "unavailable", found,
                           "discovered" if found else "not_discoverable")

    def page_size(self) -> Observation:
        try:
            size = os.sysconf("SC_PAGE_SIZE")
            if size <= 0:
                return Observation("unknown", reason="invalid_page_size")
            return Observation("available", size)
        except (AttributeError, OSError, ValueError):
            return Observation("unknown", reason="page_size_unavailable")

    def wsl_config_path(self) -> str | None:
        # Native Windows only; never expand/read another user's home.
        home = os.environ.get("USERPROFILE")
        return str(Path(home) / ".wslconfig") if home else None


def check(identifier: str, status: str, detail: str, **evidence: Any) -> dict[str, Any]:
    if status not in STATUSES:
        raise ValueError(f"Invalid check status: {status}")
    return {"id": identifier, "scope": "host_inventory", "status": status,
            "detail": detail, "evidence": evidence}


def kernel_config(probe: HostProbe, release: str) -> tuple[dict[str, str], list[dict[str, str]]]:
    attempts = []
    for path in ("/proc/config.gz", f"/boot/config-{release}"):
        result = probe.read(path, compressed=path.endswith(".gz"))
        attempts.append({"source": path, "status": result.status, "reason": result.reason})
        if result.status != "available":
            continue
        values = {}
        malformed = set()
        for line in result.value.splitlines():
            line = line.strip()
            if line.startswith("# CONFIG_") and line.endswith(" is not set"):
                key, value = line[2:-11], "n"
            else:
                key, separator, value = line.partition("=")
                if not separator:
                    continue
            if key in KERNEL_FEATURES:
                if value not in {"y", "m", "n"} or key in values:
                    malformed.add(key)
                values[key] = value
        if values:
            for key in malformed:
                values.pop(key, None)
            return values, attempts
        attempts[-1]["status"] = "unknown"
        attempts[-1]["reason"] = "no_recognized_config"
    return {}, attempts


def device_check(probe: HostProbe, path: str, identifier: str) -> dict[str, Any]:
    result = probe.path(path)
    evidence = {"path": path, "reason": result.reason}
    if result.status != "available":
        return check(identifier, result.status, "Device was not observed; no open or ioctl attempted.", **evidence)
    evidence["kind"] = result.value
    if result.value != "character_device":
        return check(identifier, "unavailable", "Path exists but is not a character device.", **evidence)
    access = probe.access(path)
    evidence["read_write_access_hint"] = access.value if access.status == "available" else None
    evidence["access_check_reason"] = access.reason
    return check(identifier, "available", "Character device exists. Access is advisory; actual device use and policy were not tested.", **evidence)


def inspect_linux(probe: HostProbe, host: dict[str, str]) -> list[dict[str, Any]]:
    checks = []
    size = probe.page_size()
    checks.append(check("linux.page_size", size.status,
                        "Recorded proof used 4096-byte pages; compatibility with an Android image is not tested.",
                        bytes=size.value, reason=size.reason))
    release = host["kernel_release"]
    checks.append(check("linux.wsl_kernel_marker", "available",
                        "Kernel-name heuristic only; WSL version and host configuration are not verified.",
                        detected=("microsoft" in release.lower() or "wsl" in release.lower())))
    values, sources = kernel_config(probe, release)
    checks.append(check("linux.kernel_config", "available" if values else "unknown",
                        "Configuration is inventory, not evidence that a module is loaded or usable.", sources=sources))
    for key, description in KERNEL_FEATURES.items():
        value = values.get(key)
        status = "available" if value in {"y", "m"} else "unavailable" if value == "n" else "unknown"
        checks.append(check("linux.kernel." + key, status, description + "; active behavior was not tested.", configured=value))
    for path, identifier in (
        ("/dev/uinput", "linux.uinput"),
        ("/dev/binder", "linux.binder"),
        ("/dev/hwbinder", "linux.hwbinder"),
        ("/dev/vndbinder", "linux.vndbinder"),
        ("/dev/binderfs/binder-control", "linux.binderfs_control"),
        ("/dev/dxg", "linux.dxg"),
    ):
        checks.append(device_check(probe, path, identifier))
    mounts = probe.read("/proc/self/mountinfo")
    mounted = set()
    valid_mounts = mounts.status == "available"
    if valid_mounts:
        rows = mounts.value.splitlines()
        if not rows:
            valid_mounts = False
        for row in rows:
            before, separator, after = row.partition(" - ")
            fields = after.split()
            if not separator or len(before.split()) < 6 or len(fields) < 3:
                valid_mounts = False
                break
            mounted.add(fields[0])
    for name, filesystems in (("binderfs", {"binder"}), ("cgroup", {"cgroup", "cgroup2"})):
        observed = sorted(mounted & filesystems) if valid_mounts else []
        status = ("available" if observed else "unavailable") if valid_mounts else "unknown"
        checks.append(check("linux.mounted." + name, status,
                            "Current process mount namespace only; mounting and container delegation were not tested.",
                            filesystems=observed, reason=mounts.reason if mounts.status != "available" else
                            "" if valid_mounts else "invalid_mountinfo"))
    for name in ("docker", "podman", "adb"):
        found = probe.tool(name)
        checks.append(check("tool." + name, found.status,
                            "Optional executable inventory only; it was not executed and no service/device was contacted.",
                            discovered=found.value, reason=found.reason))
    return checks


def inspect_windows(probe: HostProbe, host: dict[str, str]) -> list[dict[str, Any]]:
    wsl = probe.tool("wsl.exe")
    checks = [check("windows.wsl_executable", wsl.status,
                    "Executable discovery only. WSL installation, virtualization, distributions, and version are unverified.",
                    discovered=wsl.value, reason=wsl.reason)]
    config_path = probe.wsl_config_path()
    config = probe.read(config_path) if config_path else Observation("unknown", reason="user_profile_unavailable")
    if config.status == "unavailable" and config.reason == "not_found":
        checks.append(check("windows.wsl_global_config", "available",
                            "No .wslconfig file observed for the current user.",
                            present=False, kernel_override=False, kernel_modules_override=False))
    elif config.status != "available":
        checks.append(check("windows.wsl_global_config", "unknown",
                            "Global WSL configuration could not be inspected.", reason=config.reason))
    else:
        parser = configparser.ConfigParser(interpolation=None, strict=True)
        try:
            parser.read_string(config.value)
            sections = [name for name in parser.sections() if name.casefold() == "wsl2"]
            if len(sections) > 1:
                raise configparser.Error("ambiguous wsl2 section")
            values = parser[sections[0]] if sections else {}
            checks.append(check("windows.wsl_global_config", "available",
                                "WSL2 kernel overrides affect all of this user's WSL2 distributions. Values are omitted; nothing was modified.",
                                present=True, kernel_override=bool(values.get("kernel", "").strip()),
                                kernel_modules_override=bool(values.get("kernelmodules", "").strip())))
        except configparser.Error:
            checks.append(check("windows.wsl_global_config", "unknown",
                                "Global WSL configuration is not parseable; raw content is omitted.", reason="invalid_config"))
    checks.append(check("windows.wsl_runtime", "unknown",
                        "No wsl.exe commands were run. Inspect inside the intended Linux/WSL environment separately."))
    return checks


def inspect_capabilities(probe: HostProbe | None = None) -> dict[str, Any]:
    probe = probe or HostProbe()
    host = probe.metadata()
    supported = host["os"] in {"Linux", "Windows"}
    checks = [check("host.os", "available" if supported else "not_applicable",
                    "Inspector coverage only; a supported production-host matrix is not established.", os=host["os"]),
              check("host.architecture", "available" if host["architecture"] else "unknown",
                    "Architecture is recorded only; Android image and instruction-set compatibility are not tested.",
                    architecture=host["architecture"])]
    if host["os"] == "Linux":
        checks.extend(inspect_linux(probe, host))
    elif host["os"] == "Windows":
        checks.extend(inspect_windows(probe, host))
    counts = {status: sum(item["status"] == status for item in checks) for status in sorted(STATUSES)}
    return {
        "schema_version": SCHEMA_VERSION,
        "inspector": {"name": "rexplayer-capabilities", "version": INSPECTOR_VERSION, "read_only": True},
        "host": host,
        "checks": checks,
        "summary": {
            "inspection_status": "completed" if supported else "unsupported_os",
            "status_counts": counts,
            "runtime_verification": "not_performed",
            "production_readiness": "not_established",
        },
        "runtime_gates": [{"id": gate, "status": "not_tested"} for gate in RUNTIME_GATES],
        "limitations": [
            "This is a point-in-time inventory of the current process view, including container or WSL restrictions.",
            "Available means a narrow observation succeeded, not that a prerequisite is usable or the runtime works.",
            "Missing tools or device paths may reflect this namespace or PATH; alternate installations were not searched.",
            "No privileged operations, executable probes, device opens, Android commands, or network requests are performed.",
            "No report from this inspector is proof of Android boot, input, graphics, security, or production readiness.",
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pretty", action="store_true", help="Indent the JSON report for reading")
    args = parser.parse_args(argv)
    report = inspect_capabilities()
    json.dump(report, sys.stdout, indent=2 if args.pretty else None, sort_keys=True, allow_nan=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
