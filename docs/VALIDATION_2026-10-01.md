# V2 implementation checkpoint — 2026-10-01

**Result: reviewed implementation preview, not production-ready.** Work is isolated
on `codex/production-validation-20261001`, based on
`840b581828829cb76a62ad1536c6afcd27d5617d`. No merge, deployment, privileged runtime
setup, or main-branch change is part of this work.

## Implemented

- `runtime/`: read-only Linux/Windows capability inventory, versioned JSON schema,
  explicit unknown/unavailable results, and no implicit readiness assertion
- `core/input/`: bounded contact state, rotation/viewport mapping, deterministic
  slot allocation, cancellation/focus-loss/geometry-change release transitions
- `core/input-linux/`: explicit evdev/uinput device transport, direct-touch
  properties, type-B frames, independent collision-safe 16-bit kernel tracking IDs,
  latched write faults and explicit resynchronization
- `core/launcher/`: constrained Linux Waydroid Doctor/Status/Launch CLI, bounded
  diagnostics, same-user/display checks, explicit upstream session-start acknowledgement
- `core/ui/`: real GPUI 0.2.2 native shell, asynchronous diagnostics, one-shot launch
  confirmation, repeat-action guards and supervised/reaped UI-request processes
- Source gates, cross-platform CI, and reproducible unsigned Linux preview packaging

Android opens through a separately provisioned Waydroid runtime, in its own window.
The native shell does not embed Android frames. Host key capture/keymaps, app-visible
input integration, media, dedicated provisioning, signed installers, transactional
updates and full lifecycle recovery remain incomplete.

## Executed checks

Validation host: Debian 13 Linux x86_64, Rust 1.98.1, Python 3.12, GCC and PowerShell
7.6.6. GPUI is pinned to 0.2.2; evdev to 0.13.2. Dependency locks are committed.

| Check | Result and scope |
| --- | --- |
| Aggregate `python3 scripts/check.py --native-ui` | **41/41 PASS**, non-privileged source/build gates |
| General Python/evidence/packaging runner tests | **16 PASS** |
| Capability inspector | **37 PASS**; live Linux output also validates against the JSON Schema |
| Input core | **19 tests + 1 executable doctest PASS**, including 512 interrupted ten-contact cycles and 20,000 mixed operations |
| Launcher | **24 PASS**, including actual bounded subprocess fixtures |
| Linux input transport | **16 tests + 1 compile-only doctest PASS**; mocked transport, not kernel delivery |
| Missing-uinput constructor | **1 PASS**, actual constructor returns NotFound with the device absent; no device writes |
| Native shell | **22 PASS** (17 controller/worker tests plus 5 compiled-binary startup tests) |
| Native optimized build | **PASS**; real linked GPUI release binary |
| Windows-target Rust compatibility | **PASS** for applicable source/strict Clippy checks; not Windows-native runtime evidence |
| Original C tools | **PASS**, GCC `-Wall -Wextra -Werror` |
| Original Rust proof | **PASS** fmt/Clippy/build; its original test harness contains zero tests |
| Existing historical evidence | **PASS** structure and 46 manifest entries; this is not a new Android run |
| PowerShell manifest scripts | **PASS on Linux**, not Windows-native execution |
| Unsigned preview archive | **PASS** repeated-build byte equality, complete checksums, executable modes, extracted executable startup, metadata and overwrite refusal |

There are 137 passing test/doctest invocations across distinct suites, including the
separately selected missing-device check. This count includes one compile-only
doctest. Four ignored launcher fixture entrypoints are exercised by the passing
subprocess tests; the missing-device test is deliberately excluded from ordinary
hardware-independent test runs. No simulated Android result is counted as live proof.

Independent review corrected Windows-only warning/build issues, a Windows CI
failure-masking issue, and the kernel tracking-ID range assumption. The adapter
explicitly documents its dependency's zero-progress-write completion assumption;
transport completion is never described as downstream Android acknowledgement.
An upstream `proc-macro-error2 2.0.1` future-incompatibility notice remains; all checks
pass on the recorded toolchain, and the new CI pins Rust 1.98.1.

## Blocked and not run

- **Native window/visual interaction: BLOCKED.** Official Debian Xvfb, Weston and
  Mesa packages were verified/extracted without system changes. Mesa llvmpipe
  software Vulkan works, but this sandbox rejects AF_UNIX socket creation with
  `EPERM`, so a display server cannot start. No alternate transport, sandbox change,
  screenshot or keyboard/mouse-UI coverage is claimed.
- **Android/kernel/application validation: NOT_RUN.** This cloud has no usable
  Binder/uinput/KVM/GPU device environment, Docker runtime or configured Android.
  Touch events, real frames/audio, app compatibility, latency and recovery need the
  explicitly authorized supported host.
- **Windows-native WSL/Android and Linux ARM64: NOT_RUN.** Cross-compilation and
  fixtures do not replace these gates.
- **Stable release qualification: NOT_DONE.** Signing, SBOM/dependency-license review,
  install/update/rollback/uninstall, supported-hardware coverage, media/input
  endurance and 100 real lifecycle runs remain mandatory.

The August proof logs were not edited. Only source-document entries in their
existing manifest were refreshed for the updated README and roadmap.

## Next supported-host gate

Follow [PRODUCTION_VALIDATION_PLAN.md](PRODUCTION_VALIDATION_PLAN.md). First render
and exercise the native shell; then verify a same-user/display pre-provisioned
Waydroid session and an owned Android app. Record real app MotionEvent delivery,
frames/audio, repeated/aborted actions and cleanup. Windows provisioning must resolve
its isolation boundary before any global `.wslconfig` or privileged host changes.
