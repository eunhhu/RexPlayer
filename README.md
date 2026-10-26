# RexPlayer

A design-and-prototype repository for a container-native Android runtime and security-analysis platform, written in Rust (GPUI host shell) and Python (runtime tooling), aimed at people researching lightweight Android execution on Linux and Windows/WSL2 hosts.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%2011%20%7C%20Linux-blue.svg)](#tech-stack)
[![UI](https://img.shields.io/badge/Frontend-GPUI%20(Pure%20Rust%20GPU)-blue.svg)](./core/ui/README.md)
[![Engine](https://img.shields.io/badge/Engine-WSL2%20%2F%20Waydroid%20(LXC)-2496ED.svg)](./docs/ARCHITECTURE_V2.md)
[![Validation](https://img.shields.io/badge/Validation-Runtime%20proof-orange.svg)](./docs/VERIFICATION_2026-08-20.md)

**Status:** Prototype — specification-heavy pre-alpha. Android 14 boot, ADB and a raw `/dev/uinput` → Android `TOUCHSCREEN` path have been reproduced in lab runs; a production installer, graphics/audio pipeline, app-visible input, performance targets and any "undetectability" are **not** established. The recorded baseline is detectable (8 PASS / 23 FAIL / 3 SKIP in the defensive exposure matrix).

> [!IMPORTANT]
> Read the [runtime verification report](./docs/VERIFICATION_2026-08-20.md) and the [reproduction proof](./proof/README.md) before treating any architecture item below as implemented. Documents under `docs/` that describe stealth or anti-cheat topics are historical research notes, not product claims.

## Features

What exists in the tree today:

- `rex-launcher` (`core/launcher`): dependency-free Rust CLI for pre-provisioned Linux Waydroid hosts with `doctor`, `status` and an explicit-consent `launch` command.
- `rex-player` (`core/ui`): GPUI 0.2.2 window that integrates the media, keymap, input and launcher crates (Linux x86_64/aarch64, behind the `native-gpui` feature).
- `rex-media` (`core/media`): explicit-device ADB video via bounded PNG `screencap` polling or H.264 `screenrecord` decoded through a trusted local FFmpeg; separately supervised scrcpy device audio.
- `rex-keymap` (`core/keymap`): window-scoped JSON tap / hold / joystick / pointer profiles (`examples/default.json`).
- `rex-input-core` + `rex-input-linux` (`core/input`, `core/input-linux`): typed multitouch contact allocator and a Linux `/dev/uinput` type-B transport.
- Python runtime tooling (`runtime/`): read-only host capability inspector, hash-verified Android image preparation (`provision.py`), and a health-gated, atomic, rollback-capable host-package release manager (`release_manager.py`).
- Source gates (`scripts/check.py`, `scripts/check_media_codec.py`) and a packaging script for unsigned Linux previews (`scripts/package_preview.py`).
- Sanitized evidence from the 2026 lab runs plus fail-closed verification runners (`proof/`).

## How it works

The host application is a small native shell rather than a desktop emulator. On Linux, `rex-launcher` inspects an existing Waydroid session (UID, `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`) and only requests the Android UI after explicit acknowledgement. `rex-player` builds on it: `rex-media` pulls frames from an already-authorized ADB device into a newest-only mailbox, `rex-keymap` translates window-local keyboard/mouse events into touch transitions, and `rex-input-linux` submits those transitions as multitouch frames through a single virtual uinput touchscreen. Nothing is captured globally, no device is auto-selected, and input stays disabled until routing to the intended guest is verified.

On the Python side, `runtime/capability_inspector.py` inventories the host without starting anything, `runtime/provision.py` verifies local Android images and exports a review plan, and `runtime/release_manager.py` stages host packages offline, verifies hashes, activates them atomically behind a health check and rolls back on failure. The Windows path (WSL2 + custom kernel + redroid OCI image) exists only as proof scripts under `proof/windows-wsl/`.

## Tech stack

- Runtime/Language: Rust 2021/2024 editions (crates declare `rust-version` 1.82–1.88; CI pins 1.98.1), Python 3 (standard library only)
- UI: GPUI `=0.2.2`
- Input: `evdev` 0.13.2 (Linux uinput)
- Media: ADB `screencap` / `screenrecord`, external FFmpeg and scrcpy processes
- Android substrate: Waydroid / LXC on Linux, WSL2 + redroid on Windows (validation only)
- Infra/Tooling: GitHub Actions (`product-components.yml`, `runtime-proof.yml`), `cargo fmt` / `clippy -D warnings`, `unittest`

## Getting started

### Requirements

- Rust toolchain (1.88+ for `core/ui` and `core/media`; CI uses 1.98.1) with `clippy` and `rustfmt`
- Python 3
- Linux native build libraries for the GPUI shell: `build-essential pkg-config libxcb1-dev libxkbcommon-dev libxkbcommon-x11-dev libfontconfig1-dev libwayland-dev libvulkan-dev ffmpeg`
- For device media: an ADB device you have already authorized yourself

### Install

```sh
git clone https://github.com/eunhhu/RexPlayer
cd RexPlayer
```

### Run

```sh
# Python evidence / runtime tests
python -m unittest discover -s tests -v
python -m unittest discover -s runtime/tests -v

# Non-privileged source gates (add --native-ui to also compile/test the GPUI binary)
python3 scripts/check.py
python3 scripts/check.py --native-ui

# Decode a synthetic H.264 fixture through a real local FFmpeg (no device needed)
python3 scripts/check_media_codec.py

# Individual Rust components
cargo test --manifest-path core/launcher/Cargo.toml --locked
cargo build --manifest-path core/launcher/Cargo.toml --locked --release
core/launcher/target/release/rex-launcher --help

cargo build --manifest-path core/ui/Cargo.toml --locked --features native-gpui
core/ui/target/debug/rex-player --adb-serial YOUR_SERIAL
```

Each crate's README documents its own flags and limits: [`core/launcher`](./core/launcher/README.md), [`core/ui`](./core/ui/README.md), [`core/media`](./core/media/README.md), [`core/keymap`](./core/keymap/README.md), [`core/input`](./core/input/README.md), [`core/input-linux`](./core/input-linux/README.md). Runtime tooling is described in [`runtime/README.md`](./runtime/README.md), [`runtime/PROVISIONING.md`](./runtime/PROVISIONING.md) and [`runtime/RELEASE_MANAGER.md`](./runtime/RELEASE_MANAGER.md).

## Project structure

```text
core/
  launcher/     rex-launcher CLI (Waydroid doctor/status/launch)
  ui/           rex-player GPUI shell
  media/        rex-media: ADB screenshot/screenrecord video, scrcpy audio
  keymap/       rex-keymap: JSON window-scoped keymaps
  input/        rex-input-core: coordinate mapping + contact allocator
  input-linux/  rex-input-linux: /dev/uinput multitouch transport
runtime/        capability inspector, image provisioning, release manager (+ tests)
scripts/        check.py, check_media_codec.py, package_preview.py
tests/          evidence, packaging and check-runner regression tests
proof/          2026 lab evidence, input producers, Windows/WSL runners
docs/           architecture, verification reports, roadmap, research notes
.github/        CI workflows
```

## Configuration

Environment variables read by the code:

- `DISPLAY`, `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR` — checked by `rex-launcher doctor` and the GPUI shell before any window is created
- `ADB_MDNS_AUTO_CONNECT` — inspected by `rex-media` so no implicit device discovery happens
- `REX_H264_FIXTURE` — path to the generated fixture used by the `rex-media` streaming tests
- `USERPROFILE` — used by the Windows capability inspector

Keymaps are JSON files (see `core/keymap/examples/default.json`); release transactions and provisioning plans are JSON documents described in `runtime/`.

## Documentation

- [Runtime verification 2026-08-20](./docs/VERIFICATION_2026-08-20.md) and [integrated validation 2026-10-01](./docs/VALIDATION_INTEGRATED_2026-10-01.md)
- [Integrated player runbook](./docs/INTEGRATED_PLAYER.md)
- [Architecture v2 specification](./docs/ARCHITECTURE_V2.md), [input & graphics pipeline](./docs/INPUT_AND_GRAPHICS_PIPELINE.md), [hardware/audio/media pipeline](./docs/HARDWARE_AUDIO_MEDIA_PIPELINE.md)
- [Packaging and release strategy](./docs/PACKAGING_AND_RELEASE_STRATEGY.md), [production validation plan](./docs/PRODUCTION_VALIDATION_PLAN.md), [roadmap](./docs/ROADMAP.md)
- [v1 post-mortem](./docs/POST_MORTEM_V1.md) and [technical precedents](./docs/TECHNICAL_PRECEDENTS.md)
- Research notes (not product scope): [security](./docs/STEALTH_AND_SECURITY.md), [native/Zygisk](./docs/NATIVE_STEALTH_AND_ZYGISK_STRATEGY.md), [commercial anti-cheat analysis](./docs/COMMERCIAL_ANTI_CHEAT_ANALYSIS.md), [exposure matrix](./docs/COMPREHENSIVE_ANTI_DETECTION_MATRIX.md)

The original QEMU + SPICE + Qt6 implementation is archived on the [`archive/v1-qemu-spice-qt`](https://github.com/eunhhu/RexPlayer/tree/archive/v1-qemu-spice-qt) branch.

## Security and research scope

The releasable baseline is a transparent, detectable Android lab substrate. The default package must not ship hidden instrumentation, integrity bypasses, fingerprint spoofing or anti-cheat evasion; the proof collector records exposed properties instead of concealing them. Any research helper for owned-lab use must be a separate opt-in artifact with explicit risk labels.

## License

[MIT](./LICENSE). Analysis capabilities are intended for educational and security-research purposes only.
