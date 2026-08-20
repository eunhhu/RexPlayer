# Runtime feasibility proof

This directory contains a reproducible, defensive proof of the smallest RexPlayer vertical slice tested on 2026-08-20. It is test code, not the v2 product implementation.

## Verified boundary

| Check | Result |
| --- | --- |
| ARM64 Android 14 on a 4 KiB Linux guest | PASS |
| Windows 11 → WSL2 custom kernel → x86-64 Android 14 | PASS (validation-only substrate) |
| `sys.boot_completed=1`, ADB `device`, core Android processes alive | PASS |
| Linux `/dev/uinput` Type-B multitouch producer self-check | PASS |
| Android `getevent` receives all 15 emitted events | PASS on ARM64 guest and WSL2 |
| Android InputReader registers the device as `TOUCHSCREEN` / `TOUCH_SCREEN` | PASS on ARM64 guest and WSL2 |
| Baseline shell-level exposure matrix | **DETECTABLE: 8 PASS / 23 FAIL / 3 SKIP** |

A matrix PASS only means that one inspected signal was absent or matched the expected baseline. SKIP means the signal could not be inspected and is never counted as clean. Neither verdict is an anti-cheat, Play Integrity, DRM, or device-integrity attestation result.

## Layout

- `input/` — C `/dev/uinput` producer, host runner, and Android-container integration runner.
- `input-rust/` — independent Rust/`evdev` host-side producer and verifier.
- `stealth/` — defensive Android exposure collector. It observes signals; it does not hide or alter them.
- `windows-wsl/` — Windows/WSL2 preflight, custom-kernel build/apply, Android end-to-end runner, cleanup, and PowerShell checksum validation.
- `evidence/` — small, sanitized outputs from the recorded run. See `SHA256SUMS.txt` after running the repository verification command.

No VM images, container data, SSH keys, credentials, executables, or Cargo `target/` trees belong here.

## Host uinput proof

Requirements: Linux, a working `/dev/uinput`, a C compiler, and cached or passwordless sudo authorization. The runner uses `sudo -n`; run `sudo -v` first if your policy permits cached credentials.

```bash
cd proof/input
./build_and_run.sh
```

Expected final line:

```text
RESULT PASS
```

The Rust implementation can be checked independently:

```bash
cd proof/input-rust
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo build --release
sudo ./target/release/rex-input-proof
```

## Android container integration

The recorded environment was Ubuntu 22.04 ARM64 with a 4 KiB page-size kernel, `binder_linux`, `ashmem_linux`, Docker, ADB, GCC/static libc, and this pinned image:

```text
redroid/redroid@sha256:0a611199ba2e0b5d60af39b3327a517f6407231f4352114ed3bd3cbfe2be69aa
```

> [!DANGER]
> Run this only inside a disposable VM or on a dedicated lab host. `--privileged` gives the third-party Android image broad control over the Docker host, including host devices; mounting `$HOME/redroid-data` also exposes that directory to the container. A digest pins identity but does not establish trust. The recorded run used a disposable KVM guest.

A minimal lab start is:

```bash
sudo modprobe binder_linux devices=binder,hwbinder,vndbinder
sudo modprobe ashmem_linux
sudo mkdir -p /dev/binderfs
mountpoint -q /dev/binderfs || sudo mount -t binder binder /dev/binderfs

sudo docker run -d --name rexplayer-redroid-proof \
  --privileged \
  -p 127.0.0.1:5555:5555 \
  -v "$HOME/redroid-data:/data" \
  redroid/redroid@sha256:0a611199ba2e0b5d60af39b3327a517f6407231f4352114ed3bd3cbfe2be69aa \
  androidboot.redroid_gpu_mode=guest \
  androidboot.redroid_width=720 \
  androidboot.redroid_height=1280 \
  androidboot.redroid_dpi=320
```

Wait for `adb -s 127.0.0.1:5555 shell getprop sys.boot_completed` to print `1`, then run:

```bash
cd proof/input
./run_android_container_probe.sh rexplayer-redroid-proof
```

The integration runner also uses `sudo -n`; refresh allowed cached authorization with `sudo -v` before running it. Set `REX_DOCKER_HOST` when using a non-default Docker socket.

The runner builds a temporary static binary, creates a direct touchscreen with `INPUT_PROP_DIRECT`, captures Android `getevent`, and checks InputReader classification. Redroid's private `/dev` does not automatically receive the dynamic host event node in this setup, so the test-only integration path creates the matching node from the kernel-reported sysfs major/minor and removes it during cleanup.

Expected summary:

```text
GETEVENT_EVENT_LINES=15
GETEVENT_SEQUENCE=YES
INPUTREADER_REGISTERED=YES
TOUCHSCREEN_SOURCE=YES
TOUCHSCREEN_DEVICE_TYPE=YES
RESULT=PASS
```

## Windows / WSL2 validation path

A second end-to-end run used Windows 11 OpenSSH to drive an Ubuntu 24.04 WSL2 distribution. The stock WSL kernel was measured first; uinput worked, but Binder IPC was disabled. A Microsoft WSL 6.18 custom kernel built from the pinned source in [`windows-wsl/build_kernel.sh`](windows-wsl/build_kernel.sh) then provided BinderFS, uinput, and evdev as built-ins.

The recorded custom-kernel path established:

- kernel `6.18.40.1-rexplayer-wsl+`, x86-64, 4 KiB pages;
- `/dev/uinput` and `/dev/dxg` present;
- BinderFS mount plus host and container open of `binder`, `hwbinder`, and `vndbinder`;
- Docker `29.1.3` with a digest-pinned amd64 ReDroid 14 image;
- `sys.boot_completed=1`, ADB `device`, Android 14 / SDK 34 / ABI `x86_64`;
- exact 15-event Android `getevent` capture and InputReader touchscreen classification;
- the same `8 PASS / 23 FAIL / 3 SKIP`, `DETECTABLE=YES` shell-level baseline.

BinderFS files are bind-mounted into the container. Docker `--device` only recreated their device numbers and failed with `ENXIO`; it did not preserve the BinderFS inode operations. See [`windows-wsl/README.md`](windows-wsl/README.md) for the executed path and rollback boundary.

This establishes the WSL2 validation substrate, not a Windows-native Binder implementation, Android application UI handling, or Android graphics acceleration through `/dev/dxg`.

## Defensive exposure matrix

With the Android ADB endpoint available:

```bash
cd proof/stealth
./run_matrix.sh 127.0.0.1:5555
```

Exit `0` means every current inspectable check passed, exit `3` means at least one detectable exposure was recorded, and exit `4` means no failure was found but one or more checks were skipped, so the result is inconclusive. The 2026-08-20 baseline intentionally records exit `3`; see `evidence/detection-matrix-result.tsv`.

Verify the source and evidence manifest from the repository root:

```bash
python3 proof/validate_evidence.py
sha256sum -c proof/evidence/SHA256SUMS.txt
```

## Not established

This proof does **not** establish graphics or audio acceleration, frame latency, multi-instance operation, Windows-native Binder/Android execution, Android application UI handling, native app compatibility, commercial anti-cheat compatibility, Play Integrity, DRM level, OEM key attestation, sensor realism, production hardening, or “undetectability.”

Use it as a feasibility gate and regression fixture, not as a product or security claim.
