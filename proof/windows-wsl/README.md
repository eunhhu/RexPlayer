# Windows / WSL2 runtime proof

This directory contains the Windows-side feasibility probes used for RexPlayer. The path uses the Windows WSL2 environment directly; it does not add the separate Ubuntu KVM guest used by the ARM64 compatibility experiment. WSL2 itself is still a Microsoft-managed lightweight virtual machine boundary.

## Boundary

The proof has five separate gates:

1. `preflight.sh` records the active WSL kernel, page size, uinput, BinderFS, and `/dev/dxg` GPU bridge state.
2. The existing [`../input/rex_uinput_mt.c`](../input/rex_uinput_mt.c) test proves the complete 15-event Type-B multitouch sequence against the WSL kernel.
3. `build_kernel.sh` reproducibly builds a Microsoft WSL 6.18 kernel with BinderFS, uinput, and evdev built in. `set_custom_kernel.ps1` changes only the current user's `.wslconfig`, after creating a timestamped backup.
4. `run_android_e2e.sh` creates BinderFS devices, starts an isolated host-network Docker daemon, boots a digest-pinned x86-64 ReDroid 14 container, and requires Android boot plus ADB readiness.
5. The shared Android probe requires the exact 15-event sequence in Android `getevent` and `TOUCHSCREEN` / `TOUCH_SCREEN` registration in InputReader. The exposure matrix is recorded separately and is expected to report the baseline as detectable.

The stock WSL kernel must be measured before deciding whether a custom kernel is needed. Do not promote a successful uinput, Binder, or container-start gate into Android application proof.

## Preflight

Inside the target WSL distribution:

```bash
bash proof/windows-wsl/preflight.sh
```

A stock kernel with `CONFIG_ANDROID_BINDER_IPC=n` cannot host the tested Android container even when `/dev/uinput` and `/dev/dxg` are present.

## Build the validation kernel

The pinned build requires a Debian/Ubuntu WSL environment with:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential flex bison dwarves libssl-dev libelf-dev cpio bc git ca-certificates
```

Build with three jobs by default:

```bash
cd proof/windows-wsl
bash build_kernel.sh
```

The generated kernel, config, source commit, and checksums are written under `out/`; the source tree is under `.kernel-work/`. Both paths are intentionally ignored by Git.

The build is pinned to Microsoft `WSL2-Linux-Kernel` commit `14794180686c2fb6307fbe359c359bec765249f3` and fails unless these settings survive `olddefconfig`:

```text
CONFIG_ANDROID_BINDER_IPC=y
CONFIG_ANDROID_BINDERFS=y
CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder"
CONFIG_INPUT_UINPUT=y
CONFIG_INPUT_EVDEV=y
```

## Apply and verify

From a non-elevated Windows PowerShell session at the repository root:

```powershell
$kernel = (Resolve-Path .\proof\windows-wsl\out\bzImage-rexplayer-wsl).Path
$sha = (Get-FileHash $kernel -Algorithm SHA256).Hash
.\proof\windows-wsl\set_custom_kernel.ps1 -KernelPath $kernel -ExpectedSha256 $sha -Shutdown
```

The script validates the bzImage header and SHA-256, saves `.wslconfig.rexplayer-backup-<timestamp>`, changes only the `[wsl2]` `kernel=` setting, and optionally runs `wsl.exe --shutdown`. Restart the distribution and rerun:

```bash
bash proof/windows-wsl/preflight.sh
```

Expected custom-kernel gates are a `rexplayer-wsl` kernel suffix, 4 KiB pages, Binder IPC, BinderFS, uinput, and evdev set to `y`, plus `BINDERFS_MOUNTABLE=YES`.

## Run the Android end-to-end proof

Install the runtime prerequisites inside the dedicated Ubuntu WSL distribution:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends docker.io adb gcc libc6-dev ca-certificates
```

Then, from the repository root mounted inside WSL. The optional output directory must be empty and must not be a symlink:

```bash
sudo bash proof/windows-wsl/run_android_e2e.sh
# or: sudo bash proof/windows-wsl/run_android_e2e.sh /path/to/new-empty-output
```

The runner uses fixed RexPlayer-owned socket/data paths and refuses to share an existing BinderFS mount. It binds ownership to the BinderFS mount ID and Android container ID; cleanup is mandatory and fails unless the container is removed, the dedicated daemon is inactive, its cgroup is gone, TCP 5555 is closed, and the recorded mount ID is unmounted. Child Bash processes receive no `BASH_ENV`/`ENV`, and final end-to-end PASS markers are emitted only after cleanup succeeds. No environment variable can turn a successful end-to-end run into a retained privileged runtime. A stale marker cannot authorize removal of a replacement mount. It uses host networking because the minimal WSL validation kernel does not include the normal Docker bridge module stack. ADB on TCP 5555 and the broad proof privileges are laboratory-only; run this only on an isolated trusted host. It boots this pinned amd64 image:

```text
redroid/redroid@sha256:11d58a64bfbde2253d1cce81bff409ff58174980222d1bada232d9ef59181191
```

BinderFS nodes must be bind-mounted as files. Docker `--device` recreates only their major/minor numbers, loses BinderFS inode operations, and produced `ENXIO` (`No such device or address`) in the recorded run. `run_redroid.sh` therefore uses `type=bind` mounts for `binder`, `hwbinder`, and `vndbinder`.

Expected successful terminal markers are:

```text
ANDROID_BOOT=PASS
ADB_STATE=device
GETEVENT_SEQUENCE=YES
INPUTREADER_REGISTERED=YES
TOUCHSCREEN_SOURCE=YES
TOUCHSCREEN_DEVICE_TYPE=YES
RESULT=PASS
WINDOWS_WSL_ANDROID_INPUT=PASS
WINDOWS_WSL_ANDROID_MATRIX=CAPTURED
```

The 2026-08-20 baseline matrix exits `3` by design and records `8 PASS / 23 FAIL / 3 SKIP`, `DETECTABLE=YES`. Raw output is written below `proof/windows-wsl/out/` and stays ignored. Commit only reviewed, sanitized evidence.

Cleanup without deleting the image cache:

```bash
sudo bash proof/windows-wsl/cleanup_android_host.sh
```

Pass `--purge-data` to remove the isolated Docker image/data cache too.

## Windows-native checksum verification

From a Windows PowerShell checkout:

```powershell
.\proof\windows-wsl\verify_evidence.ps1
.\proof\windows-wsl\test_verify_evidence.ps1
```

The verifier checks every GNU-style `SHA256SUMS.txt` row with `Get-FileHash` and does not require Python or WSL. It rejects rooted paths, `..`, Windows ADS syntax, and symlink/junction components at every path level. The negative test exercises traversal, rooted paths, and ancestor reparse points. `.gitattributes` forces LF checkout for every hashed text file on Windows and Linux.

## Rollback

Stop the distribution, restore the timestamped backup reported by `set_custom_kernel.ps1`, then start WSL again:

```powershell
wsl.exe --shutdown
$backup = Get-ChildItem "$env:USERPROFILE\.wslconfig.rexplayer-backup-*" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Copy-Item $backup.FullName "$env:USERPROFILE\.wslconfig" -Force
```

Verify the restored kernel with `wsl.exe --status` and `uname -r` inside the distribution. Do not delete the backup until rollback has been exercised.

## Safety and claim limits

A custom WSL kernel affects every WSL2 distribution owned by the current Windows user after the next WSL shutdown. A privileged Android container can access host kernel devices and must only be used on a dedicated lab machine. The recorded pass establishes Android boot/ADB and kernel-to-Android InputReader delivery, not application UI handling, `/dev/dxg` Android graphics acceleration, latency, audio/media, anti-cheat compatibility, integrity attestation, DRM, or undetectability. WSL2 remains a Microsoft-managed lightweight VM boundary and this directory does not establish a Windows-native Binder runtime. Production artifact separation, installer boundaries, network isolation, signing, updates, and rollback gates are specified in [`../../docs/PACKAGING_AND_RELEASE_STRATEGY.md`](../../docs/PACKAGING_AND_RELEASE_STRATEGY.md).
