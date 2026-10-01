"""Deterministic tests; platform fixtures do not imply native Windows validation."""

from __future__ import annotations

import ast
import contextlib
import gzip
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import capability_inspector as inspector

O = inspector.Observation


class FakeProbe(inspector.HostProbe):
    def __init__(self, system="Linux"):
        self.system = system
        self.release = "test-kernel"
        self.files = {}
        self.paths = {}
        self.tools = {}
        self.accesses = {}
        self.size = O("available", 4096)
        self.config_path = "current-user/.wslconfig"
        self.calls = []

    def metadata(self):
        return {"os": self.system, "architecture": "x86_64",
                "kernel_release": self.release, "os_version": "fixture"}

    def read(self, path, *, compressed=False):
        self.calls.append(("read", path, compressed))
        return self.files.get(path, O("unavailable", reason="not_found"))

    def path(self, path):
        self.calls.append(("path", path))
        return self.paths.get(path, O("unavailable", reason="not_found"))

    def access(self, path):
        self.calls.append(("access", path))
        return self.accesses.get(path, O("available", False))

    def tool(self, name):
        self.calls.append(("tool", name))
        return self.tools.get(name, O("unavailable", False, "not_discoverable"))

    def page_size(self):
        return self.size

    def wsl_config_path(self):
        return self.config_path


def by_id(report):
    return {item["id"]: item for item in report["checks"]}


class InspectorTests(unittest.TestCase):
    def test_unsupported_os_does_no_platform_probes(self):
        probe = FakeProbe("Darwin")
        report = inspector.inspect_capabilities(probe)
        self.assertEqual(report["summary"]["inspection_status"], "unsupported_os")
        self.assertEqual(probe.calls, [])
        self.assertEqual(by_id(report)["host.os"]["status"], "not_applicable")

    def test_missing_config_is_unknown_not_disabled(self):
        checks = by_id(inspector.inspect_capabilities(FakeProbe()))
        self.assertEqual(checks["linux.kernel_config"]["status"], "unknown")
        for key in inspector.KERNEL_FEATURES:
            self.assertEqual(checks["linux.kernel." + key]["status"], "unknown")
        self.assertEqual(checks["linux.uinput"]["status"], "unavailable")

    def test_missing_tools_are_explicit_and_not_executed(self):
        probe = FakeProbe()
        checks = by_id(inspector.inspect_capabilities(probe))
        for name in ("docker", "podman", "adb"):
            self.assertEqual(checks["tool." + name]["status"], "unavailable")
            self.assertFalse(checks["tool." + name]["evidence"]["discovered"])
        self.assertTrue(all(call[0] in {"read", "path", "tool", "access"} for call in probe.calls))

    def test_permission_failures_are_not_missing_capabilities(self):
        probe = FakeProbe()
        probe.files["/proc/config.gz"] = O("unknown", reason="permission_denied")
        probe.paths["/dev/uinput"] = O("unknown", reason="permission_denied")
        checks = by_id(inspector.inspect_capabilities(probe))
        self.assertEqual(checks["linux.kernel_config"]["status"], "unknown")
        self.assertEqual(checks["linux.uinput"]["status"], "unknown")
        self.assertEqual(checks["linux.uinput"]["evidence"]["reason"], "permission_denied")

    def test_fallback_config_and_yes_module_disabled_values(self):
        probe = FakeProbe()
        probe.files["/proc/config.gz"] = O("unknown", reason="permission_denied")
        probe.files["/boot/config-test-kernel"] = O("available", "\n".join((
            "CONFIG_ANDROID_BINDER_IPC=y", "CONFIG_INPUT_UINPUT=m",
            "# CONFIG_ANDROID_BINDERFS is not set", "CONFIG_INPUT_EVDEV=n")))
        checks = by_id(inspector.inspect_capabilities(probe))
        self.assertEqual(checks["linux.kernel.CONFIG_ANDROID_BINDER_IPC"]["status"], "available")
        self.assertEqual(checks["linux.kernel.CONFIG_INPUT_UINPUT"]["evidence"]["configured"], "m")
        self.assertEqual(checks["linux.kernel.CONFIG_ANDROID_BINDERFS"]["status"], "unavailable")
        self.assertEqual(checks["linux.kernel.CONFIG_INPUT_EVDEV"]["status"], "unavailable")
        self.assertEqual(len(checks["linux.kernel_config"]["evidence"]["sources"]), 2)

    def test_duplicate_or_invalid_kernel_value_is_unknown(self):
        probe = FakeProbe()
        probe.files["/proc/config.gz"] = O("available", "CONFIG_INPUT_UINPUT=y\nCONFIG_INPUT_UINPUT=n\nCONFIG_INPUT_EVDEV=maybe")
        checks = by_id(inspector.inspect_capabilities(probe))
        self.assertEqual(checks["linux.kernel.CONFIG_INPUT_UINPUT"]["status"], "unknown")
        self.assertEqual(checks["linux.kernel.CONFIG_INPUT_EVDEV"]["status"], "unknown")

    def test_unrecognized_config_uses_fallback(self):
        probe = FakeProbe()
        probe.files["/proc/config.gz"] = O("available", "not a kernel config")
        probe.files["/boot/config-test-kernel"] = O("available", "CONFIG_INPUT_UINPUT=y")
        values, attempts = inspector.kernel_config(probe, probe.release)
        self.assertEqual(values["CONFIG_INPUT_UINPUT"], "y")
        self.assertEqual(attempts[0]["reason"], "no_recognized_config")

    def test_character_device_access_is_advisory_and_never_opened(self):
        probe = FakeProbe()
        probe.paths["/dev/uinput"] = O("available", "character_device")
        probe.accesses["/dev/uinput"] = O("available", False)
        checks = by_id(inspector.inspect_capabilities(probe))
        self.assertEqual(checks["linux.uinput"]["status"], "available")
        self.assertFalse(checks["linux.uinput"]["evidence"]["read_write_access_hint"])
        self.assertNotIn(("read", "/dev/uinput", False), probe.calls)

    def test_wrong_device_type_is_unavailable(self):
        probe = FakeProbe()
        probe.paths["/dev/uinput"] = O("available", "regular_file")
        checks = by_id(inspector.inspect_capabilities(probe))
        self.assertEqual(checks["linux.uinput"]["status"], "unavailable")
        self.assertNotIn(("access", "/dev/uinput"), probe.calls)

    def test_mountinfo_inventory(self):
        probe = FakeProbe()
        probe.files["/proc/self/mountinfo"] = O("available", "29 1 0:27 / /dev/binderfs rw - binder binder rw\n30 1 0:28 / /sys/fs/cgroup ro - cgroup2 cgroup rw\n")
        checks = by_id(inspector.inspect_capabilities(probe))
        self.assertEqual(checks["linux.mounted.binderfs"]["status"], "available")
        self.assertEqual(checks["linux.mounted.cgroup"]["evidence"]["filesystems"], ["cgroup2"])

    def test_malformed_empty_or_unreadable_mountinfo_is_unknown(self):
        for content in (O("available", ""), O("available", "wrong - binder"), O("unknown", reason="permission_denied")):
            with self.subTest(content=content):
                probe = FakeProbe()
                probe.files["/proc/self/mountinfo"] = content
                checks = by_id(inspector.inspect_capabilities(probe))
                self.assertEqual(checks["linux.mounted.binderfs"]["status"], "unknown")

    def test_wsl_kernel_detection_is_heuristic(self):
        probe = FakeProbe()
        probe.release = "6.6.0-microsoft-standard-WSL2"
        checks = by_id(inspector.inspect_capabilities(probe))
        self.assertTrue(checks["linux.wsl_kernel_marker"]["evidence"]["detected"])

    def test_page_size_failure_is_unknown(self):
        probe = FakeProbe()
        probe.size = O("unknown", reason="page_size_unavailable")
        checks = by_id(inspector.inspect_capabilities(probe))
        self.assertEqual(checks["linux.page_size"]["status"], "unknown")

    def test_windows_missing_wsl_does_not_probe_or_launch_linux(self):
        probe = FakeProbe("Windows")
        checks = by_id(inspector.inspect_capabilities(probe))
        self.assertEqual(checks["windows.wsl_executable"]["status"], "unavailable")
        self.assertEqual(checks["windows.wsl_runtime"]["status"], "unknown")
        self.assertEqual(probe.calls, [("tool", "wsl.exe"), ("read", "current-user/.wslconfig", False)])

    def test_windows_kernel_override_redacts_values(self):
        probe = FakeProbe("Windows")
        probe.tools["wsl.exe"] = O("available", True, "discovered")
        probe.files[probe.config_path] = O("available", "[wsl2]\nkernel=C:\\Users\\PrivateName\\secret-kernel\nkernelModules=C:\\private-modules\nmemory=8GB\n")
        report = inspector.inspect_capabilities(probe)
        check = by_id(report)["windows.wsl_global_config"]
        self.assertTrue(check["evidence"]["kernel_override"])
        self.assertTrue(check["evidence"]["kernel_modules_override"])
        serialized = json.dumps(report)
        for secret in ("PrivateName", "secret-kernel", "private-modules", "8GB", "current-user"):
            self.assertNotIn(secret, serialized)

    def test_windows_missing_config_is_distinct_from_unreadable(self):
        probe = FakeProbe("Windows")
        check = by_id(inspector.inspect_capabilities(probe))["windows.wsl_global_config"]
        self.assertFalse(check["evidence"]["present"])
        probe.files[probe.config_path] = O("unknown", reason="permission_denied")
        check = by_id(inspector.inspect_capabilities(probe))["windows.wsl_global_config"]
        self.assertEqual(check["status"], "unknown")
        self.assertNotIn("kernel_override", check["evidence"])

    def test_windows_missing_profile_does_not_read_relative_file(self):
        probe = FakeProbe("Windows")
        probe.config_path = None
        checks = by_id(inspector.inspect_capabilities(probe))
        self.assertEqual(checks["windows.wsl_global_config"]["status"], "unknown")
        self.assertFalse(any(call[0] == "read" for call in probe.calls))

    def test_windows_invalid_config_is_unknown_and_redacted(self):
        for data in ("secret invalid data", "[wsl2]\nkernel=private\nkernel=other", "[wsl2]\nkernel=private\n[WSL2]\nkernel=other"):
            with self.subTest(data=data):
                probe = FakeProbe("Windows")
                probe.files[probe.config_path] = O("available", data)
                report = inspector.inspect_capabilities(probe)
                self.assertEqual(by_id(report)["windows.wsl_global_config"]["status"], "unknown")
                self.assertNotIn("private", json.dumps(report))

    def test_report_contract_and_no_readiness_claim(self):
        for system in ("Linux", "Windows", "Darwin"):
            report = inspector.inspect_capabilities(FakeProbe(system))
            self.assertEqual(report["schema_version"], "1.0")
            self.assertEqual(set(report), {"schema_version", "inspector", "host", "checks", "summary", "runtime_gates", "limitations"})
            self.assertTrue(report["inspector"]["read_only"])
            ids = [check["id"] for check in report["checks"]]
            self.assertEqual(len(ids), len(set(ids)))
            for check in report["checks"]:
                self.assertEqual(set(check), {"id", "scope", "status", "detail", "evidence"})
                self.assertIn(check["status"], inspector.STATUSES)
                self.assertEqual(check["scope"], "host_inventory")
            counts = report["summary"]["status_counts"]
            self.assertEqual(sum(counts.values()), len(report["checks"]))
            for status, count in counts.items():
                self.assertEqual(count, sum(check["status"] == status for check in report["checks"]))
            self.assertEqual(report["summary"]["production_readiness"], "not_established")
            self.assertEqual(report["summary"]["runtime_verification"], "not_performed")
            self.assertTrue(all(gate["status"] == "not_tested" for gate in report["runtime_gates"]))
            json.dumps(report, allow_nan=False)

    def test_published_schema_matches_contract(self):
        schema = json.loads((Path(inspector.__file__).parent / "capability-report.schema.json").read_text())
        self.assertEqual(schema["properties"]["schema_version"]["const"], inspector.SCHEMA_VERSION)
        self.assertEqual(set(schema["$defs"]["status"]["enum"]), inspector.STATUSES)
        gate_schema = schema["properties"]["runtime_gates"]
        self.assertEqual(set(gate_schema["items"]["properties"]["id"]["enum"]), set(inspector.RUNTIME_GATES))
        self.assertEqual(gate_schema["minItems"], len(inspector.RUNTIME_GATES))
        report = inspector.inspect_capabilities(FakeProbe())
        self.assertEqual(set(schema["required"]), set(report))

    def test_command_emits_only_json_and_zero_means_report_completed(self):
        expected = inspector.inspect_capabilities(FakeProbe("unsupported"))
        output = io.StringIO()
        with patch.object(inspector, "inspect_capabilities", return_value=expected), contextlib.redirect_stdout(output):
            self.assertEqual(inspector.main(["--pretty"]), 0)
        self.assertEqual(json.loads(output.getvalue()), expected)

    def test_invalid_arguments_fail(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exc:
            inspector.main(["--provision"])
        self.assertEqual(exc.exception.code, 2)

    def test_inspector_has_no_process_network_or_mutation_imports(self):
        tree = ast.parse(Path(inspector.__file__).read_text())
        modules = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                modules.update(item.name.split(".")[0] for item in node.names)
            elif isinstance(node, ast.ImportFrom):
                modules.add(node.module.split(".")[0])
        self.assertFalse(modules & {"subprocess", "socket", "http", "urllib", "ctypes", "winreg"})
        self.assertNotIn("os.system", Path(inspector.__file__).read_text())


class HostProbeTests(unittest.TestCase):
    def setUp(self):
        self.probe = inspector.HostProbe()
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.file = Path(self.temp.name) / "config"

    def test_regular_and_bom_text(self):
        self.file.write_bytes(b"\xef\xbb\xbfCONFIG_INPUT_UINPUT=y\n")
        result = self.probe.read(str(self.file))
        self.assertEqual(result, O("available", "CONFIG_INPUT_UINPUT=y\n"))

    def test_gzip_config(self):
        self.file.write_bytes(gzip.compress(b"CONFIG_INPUT_UINPUT=y\n"))
        self.assertEqual(self.probe.read(str(self.file), compressed=True).value, "CONFIG_INPUT_UINPUT=y\n")

    def test_malformed_gzip_and_utf8_are_unknown(self):
        for data, compressed in ((b"wrong", True), (b"\x1f\x8b", True), (b"\xff", False)):
            with self.subTest(data=data):
                self.file.write_bytes(data)
                self.assertEqual(self.probe.read(str(self.file), compressed=compressed).reason, "invalid_content")

    def test_decompression_failure_is_unknown(self):
        self.file.write_bytes(b"placeholder")
        with patch.object(inspector.gzip, "GzipFile", side_effect=inspector.zlib.error("corrupt")):
            self.assertEqual(self.probe.read(str(self.file), compressed=True).reason, "invalid_content")

    def test_plain_and_decompressed_size_limits(self):
        with patch.object(inspector, "MAX_SOURCE_BYTES", 32):
            for compressed in (False, True):
                self.file.write_bytes(gzip.compress(b"x" * 33) if compressed else b"x" * 33)
                self.assertEqual(self.probe.read(str(self.file), compressed=compressed).reason, "source_too_large")

    def test_missing_file_and_permission_denied_distinguished(self):
        self.assertEqual(self.probe.read(str(self.file)).status, "unavailable")
        with patch.object(inspector.os, "lstat", side_effect=PermissionError()):
            self.assertEqual(self.probe.read(str(self.file)), O("unknown", reason="permission_denied"))
        with patch.object(inspector.os, "stat", side_effect=PermissionError()):
            self.assertEqual(self.probe.path(str(self.file)).status, "unknown")

    def test_directories_rejected_before_open(self):
        with patch.object(inspector.os, "open", side_effect=AssertionError("must not open")):
            self.assertEqual(self.probe.read(self.temp.name).reason, "not_regular_file")

    @unittest.skipUnless(hasattr(os, "mkfifo"), "POSIX-only FIFO fixture")
    def test_fifo_rejected_without_open_or_blocking(self):
        os.mkfifo(self.file)
        with patch.object(inspector.os, "open", side_effect=AssertionError("must not open FIFO")):
            self.assertEqual(self.probe.read(str(self.file)).reason, "not_regular_file")

    @unittest.skipUnless(os.name == "posix", "POSIX symlink fixture")
    def test_symlink_config_is_unknown_without_following(self):
        target = Path(self.temp.name) / "secret"
        target.write_text("private config")
        self.file.symlink_to(target)
        with patch.object(inspector.os, "open", side_effect=AssertionError("must not follow")):
            self.assertEqual(self.probe.read(str(self.file)).reason, "not_regular_file")

    def test_tool_discovery_never_returns_paths(self):
        with patch.object(inspector.shutil, "which", return_value="/private/path/adb"):
            result = self.probe.tool("adb")
            self.assertEqual(result, O("available", True, "discovered"))

    def test_tool_discovery_failure_is_unknown(self):
        with patch.object(inspector.shutil, "which", side_effect=PermissionError()):
            self.assertEqual(self.probe.tool("adb").status, "unknown")

    def test_invalid_page_size_is_unknown(self):
        with patch.object(inspector.os, "sysconf", return_value=-1, create=True):
            self.assertEqual(self.probe.page_size().status, "unknown")

    def test_access_check_unsupported_is_unknown(self):
        with patch.object(inspector.os, "access", side_effect=NotImplementedError()):
            self.assertEqual(self.probe.access("fixture").status, "unknown")

    def test_file_descriptor_closed_if_fstat_fails(self):
        self.file.write_text("CONFIG_INPUT_UINPUT=y")
        with patch.object(inspector.os, "fstat", side_effect=OSError()), patch.object(inspector.os, "close", wraps=os.close) as close:
            self.assertEqual(self.probe.read(str(self.file)).status, "unknown")
            close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
