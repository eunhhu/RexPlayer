# RexPlayer Runtime Verification — 2026-08-20

## Executive result

A minimal Android substrate and `/dev/uinput` touchscreen path were exercised end to end on two validation substrates: an ARM64 4 KiB KVM guest and Windows 11 WSL2 with a custom x86-64 kernel. Both feasibility gates passed, but the baseline Android environment is plainly detectable and does not support an “undetectable” claim.

Repository baseline under test: `c247c635c0e2172cb2038a7a912130c662a74e3f`.

| Area | Verdict | Direct evidence |
| --- | --- | --- |
| ARM64 Android substrate | PASS | Android 14 / SDK 34, `sys.boot_completed=1`, ADB `device`, core processes alive |
| Windows 11 WSL2 Android substrate | PASS | custom WSL 6.18 kernel, BinderFS, ReDroid 14 x86-64, boot/ADB ready |
| Linux uinput producer | PASS | 15 events, 3 `SYN_REPORT`s, complete down/move/up sequence |
| Android raw input receipt | PASS on both substrates | Android `getevent` recorded the same 15 events |
| Android InputReader integration | PASS on both substrates | Device registered as `Sources: TOUCHSCREEN`, `DeviceType: TOUCH_SCREEN` |
| Shell-level exposure matrix | FAIL / detectable | 8 checks passed, 23 failed, 3 skipped as unavailable |
| Product-level v2 claims | NOT VERIFIED | No v2 renderer, audio, lifecycle, multi-instance, integrity, or anti-cheat implementation was exercised |

## Scope and safety

This work is an owned-lab defensive validation. The exposure matrix reads properties, procfs, mounts, process/maps/port signals, root artifacts, ADB state, graphics strings, and device identity signals. It does not spoof integrity, hide instrumentation, patch an anti-cheat, or implement detection bypasses.

## Substrate path

### Direct host attempt

The Raspberry Pi ARM64 host uses a 16 KiB page-size kernel. Direct ReDroid attempts did not boot in the tested combination:

- Android 14 container exited with code `139`.
- Android 16 container exited with code `127` and reported `WriteProtected mprotect 1 failed: Invalid argument`.

Those results only establish incompatibility of the tested images/configuration with this 16 KiB host. They do not establish that Android or ReDroid generally cannot run on 16 KiB systems.

### Working 4 KiB guest

A KVM-accelerated Ubuntu 22.04 ARM64 guest provided the working substrate:

- kernel: `5.15.0-187-generic`
- architecture: `aarch64`
- page size: `4096`
- `binder_linux`, `ashmem_linux`, and binderfs active
- Docker `29.1.3`
- image: `redroid/redroid@sha256:0a611199ba2e0b5d60af39b3327a517f6407231f4352114ed3bd3cbfe2be69aa`
- Android 14, SDK 34, ABI `arm64-v8a`
- `sys.boot_completed=1`
- ADB state `device`
- `surfaceflinger`, `adbd`, `zygote64`, and `system_server` alive

The privileged third-party image was confined to this disposable guest. Reproducing it directly on a workstation is not recommended: image digest pinning identifies the tested bytes but is not a trust guarantee, and `--privileged` exposes the Docker host and its devices.

The exact non-secret environment capture is in [`proof/evidence/environment.txt`](../proof/evidence/environment.txt).

### Working Windows / WSL2 path

Windows OpenSSH drove an Ubuntu 24.04 WSL2 distribution directly; no additional KVM guest was introduced. WSL2 itself remains a Microsoft-managed lightweight VM boundary.

The stock WSL kernel passed the host uinput proof but had `CONFIG_ANDROID_BINDER_IPC=n`. A custom kernel built from pinned Microsoft WSL 6.18 source then passed these gates:

- kernel `6.18.40.1-rexplayer-wsl+`, x86-64, page size `4096`;
- Binder IPC, BinderFS, uinput, and evdev built in;
- `/dev/uinput` and the `/dev/dxg` bridge present;
- host BinderFS nodes openable with `O_RDWR`;
- the same nodes openable in a container when bind-mounted as BinderFS files;
- isolated Docker `29.1.3` daemon using host networking;
- pinned image `redroid/redroid@sha256:11d58a64bfbde2253d1cce81bff409ff58174980222d1bada232d9ef59181191`, architecture `amd64`;
- Android 14 / SDK 34 / ABI `x86_64`, `sys.boot_completed=1`, ADB `device`;
- exact 15-event Android `getevent` sequence and InputReader touchscreen classification.

A key failure signature was resolved during the run: Docker `--device` recreated BinderFS node numbers but not BinderFS inode operations, so Android services received `ENXIO` and repeatedly aborted. File bind mounts for `binder`, `hwbinder`, and `vndbinder` preserved the operations and allowed boot to complete.

The sanitized WSL2 runtime, input, and matrix evidence is under [`proof/evidence/`](../proof/evidence/), prefixed `windows-wsl-`. The reproducible runner and rollback boundary are in [`proof/windows-wsl/README.md`](../proof/windows-wsl/README.md). The GNU-style evidence manifest was also verified from Windows-native PowerShell with `Get-FileHash`; that path does not depend on WSL or Python.

This pass does not establish Windows-native Binder, Android UI-level touch handling, WSLg/D3D12 Android rendering, graphics performance, audio, latency, or production packaging.

## `/dev/uinput` touchscreen proof

The C proof creates a Type-B multitouch device with:

- `BTN_TOUCH`
- `ABS_MT_SLOT`
- `ABS_MT_TRACKING_ID`
- `ABS_MT_POSITION_X/Y`
- compatibility `ABS_X/Y`
- `INPUT_PROP_DIRECT`

It emits a down at `(100, 200)`, a move to `(400, 500)`, and an up (`tracking id = -1`). The producer opens the resulting event device and rejects the run unless it observes all expected values and at least three `SYN_REPORT`s.

Recorded producer result:

```text
SUMMARY events=15 expected=15 syn_reports=3 sequence=1
RESULT PASS
```

For the Android integration run, the static producer executed inside the privileged ReDroid lab container. Android `getevent` captured 15 lines matching the emitted sequence, and `dumpsys input` reported:

```text
Classes: TOUCH | TOUCH_MT | EXTERNAL
Sources: TOUCHSCREEN
DeviceType: TOUCH_SCREEN
```

The integration summary is:

```text
PRODUCER_EXIT=0
PRODUCER_PASS=YES
GETEVENT_EVENT_LINES=15
GETEVENT_SEQUENCE=YES
INPUTREADER_REGISTERED=YES
TOUCHSCREEN_SOURCE=YES
TOUCHSCREEN_DEVICE_TYPE=YES
RESULT=PASS
```

This proves the kernel producer → event node → Android raw reader → Android InputReader classification path. It does not measure end-to-end application latency or prove that every Android app accepts the synthesized input.

## Exposure matrix result

The recorded matrix contains 34 evaluated checks plus informational rows:

- PASS: 8
- FAIL: 23
- SKIP: 3 (`ro.kernel.qemu` unavailable, `/proc/cmdline` unreadable/absent, and incomplete `/proc/*/maps` inspection)
- verdict: `DETECTABLE=YES`

High-signal failures include:

- `ro.hardware=redroid` and related Redroid product/brand/model values
- `userdebug`, `test-keys`, and `ro.debuggable=1`
- insecure ADB configuration
- missing verified-boot/locked-state signals
- overlay/container mount paths visible through procfs
- `/system/xbin/su` and `/data/adb`
- Redroid/ANGLE/SwiftShader graphics strings
- no telephony identity and an empty serial signal

Frida process-name, conventional Frida ports `27042/27043`, and readable-map name checks passed because no Frida process was installed or running. Those passes are not evidence that hidden instrumentation would evade detection.

The complete sanitized table is in [`proof/evidence/detection-matrix-result.tsv`](../proof/evidence/detection-matrix-result.tsv).

## Evidence and reproducibility

Reproduction source and runners live under [`proof/`](../proof/). Small captured outputs are committed under `proof/evidence/`; VM images, container data, SSH keys, binaries, and Cargo `target/` are excluded.

The proof scripts are designed to fail closed:

- C and Rust producers return nonzero unless the complete normalized 15-event sequence matches exactly.
- The Android integration runner requires producer success, the exact Android down/move/up sequence, and `TOUCHSCREEN` / `TOUCH_SCREEN` classification inside the named proof-device block.
- The exposure runner validates the complete check set and returns `3` on a detected exposure or `4` when skipped checks make an otherwise clean run inconclusive.

## Product decision

The test supports continuing the Android substrate and input bridge work. It does not support presenting the current v2 branch as implemented, production-ready, benchmarked, or undetectable.

The next product gates require separately reproducible evidence for graphics/compositor output, audio, application-visible touch, latency, lifecycle recovery, multi-instance isolation, production Windows packaging, and any integrity/security claims. Until then, the corresponding architecture documents are design targets rather than verified capabilities.
