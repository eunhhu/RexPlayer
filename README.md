# RexPlayer v2: Next-Gen Ultra-Lightweight Android Engine & Security Analysis Platform

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%2011%20%7C%20Linux-blue.svg)](#supported-platforms)
[![UI](https://img.shields.io/badge/Frontend-GPUI%20(Pure%20Rust%20GPU)-blue.svg)](#frontend--ui)
[![Engine](https://img.shields.io/badge/Engine-WSL2%20%2F%20Waydroid%20(LXC)-2496ED.svg)](#engine--virtualization)
[![Validation](https://img.shields.io/badge/Validation-Runtime%20proof-orange.svg)](./docs/VERIFICATION_2026-08-20.md)

> **RexPlayer v2** is an active design and prototype for a container-native Android execution and security-analysis platform.
> It targets LXC/Waydroid-style Android substrates, native input, and accelerated graphics on Windows and Linux. Performance, production readiness, commercial anti-cheat compatibility, and “undetectability” are not currently established.

> [!IMPORTANT]
> The current `main` branch is specification-heavy pre-alpha work, not a finished v2 implementation. A 2026-08-20 ARM64 lab run and a separate Windows 11 → WSL2 custom-kernel run both proved Android 14 boot, ADB, and a `/dev/uinput` → Android `TOUCHSCREEN` path, while the baseline environment remained **detectable (8 PASS / 23 FAIL / 3 SKIP)**. Read the [runtime verification report](./docs/VERIFICATION_2026-08-20.md) and [reproduction proof](./proof/README.md) before treating any architecture item below as implemented.

---

## 🧭 Why RexPlayer v2?

RexPlayer investigates whether a small native host application can manage an Android container substrate without inheriting a traditional desktop-emulator stack. The project is evidence-led: a capability moves from design to implementation only after a reproducible runtime gate passes.

---

## ⚡ Key Architectural Highlights

| Area | Current evidence | Product status |
| :--- | :--- | :--- |
| **Windows substrate** | Windows 11 → WSL2 custom kernel → x86-64 Android 14 booted with ADB | Validation-only; installer and lifecycle recovery are not implemented |
| **Linux substrate** | ARM64 Android booted in a 4 KiB KVM validation guest | Native distro packaging and hardware coverage are not established |
| **Input** | Exact 15-event `/dev/uinput` sequence reached Android `getevent` and InputReader as `TOUCHSCREEN` | Raw path verified; application/UI delivery and latency remain open gates |
| **Graphics/audio** | WSL `/dev/dxg` presence was recorded | Rendering, audio, frame pacing, and zero-copy claims are unverified |
| **Host UI** | GPUI architecture documents only | Not implemented or benchmarked |
| **Security posture** | Defensive exposure collector reports `8 PASS / 23 FAIL / 3 SKIP` | Baseline is detectable; no bypass or undetectability claim |
| **Resource usage** | No controlled benchmark | RAM, CPU, startup time, and multi-instance targets are unverified |

---

## 🏛️ System Architecture

```text
Signed native launcher (planned)
        │
        ├── Windows: dedicated WSL distribution + pinned custom kernel
        │                │
        │                └── pinned Android OCI image + BinderFS/uinput
        │
        └── Linux: distro package + supported host-kernel capability gate
                         │
                         └── pinned Android OCI image + BinderFS/uinput

Verified today: Android boot/ADB and raw uinput → InputReader path
Not yet verified: production installer, graphics, audio, app-visible input,
updates, multi-instance isolation, performance, or broad hardware support
```

---

## 🛡️ Security and research scope

RexPlayer's releasable baseline is a transparent, detectable Android lab substrate. The default package must not ship hidden instrumentation, integrity bypasses, fingerprint spoofing, or anti-cheat evasion. Research helpers, if developed for authorized owned-lab use, must be separate opt-in artifacts with explicit risk labels and must never be represented as an undetectable consumer feature.

The current proof deliberately records exposed properties, root artifacts, ADB state, graphics markers, and container fingerprints instead of changing or concealing them.

---

## 📚 Technical Documentation

Explore the detailed architecture and technical specifications in [`/docs`](./docs):

- **[Runtime Verification 2026-08-20 (`docs/VERIFICATION_2026-08-20.md`)](./docs/VERIFICATION_2026-08-20.md)**: Executed Android boot, ADB, `/dev/uinput` touchscreen, and defensive exposure-matrix evidence.
- **[Reproduction Proof (`proof/README.md`)](./proof/README.md)**: Source, fail-closed runners, sanitized logs, and explicit verification boundaries.
- **[Packaging and Release Strategy (`docs/PACKAGING_AND_RELEASE_STRATEGY.md`)](./docs/PACKAGING_AND_RELEASE_STRATEGY.md)**: Artifact split, Windows/Linux installers, WSL kernel risk, updates/rollback, signing, SBOM/provenance, CI channels, and mandatory release gates.
- **[Technical Precedents & Prior Art (`docs/TECHNICAL_PRECEDENTS.md`)](./docs/TECHNICAL_PRECEDENTS.md)**: Background research on Android execution approaches and prior implementation trade-offs.
- **[RexPlayer v1 Post-Mortem (`docs/POST_MORTEM_V1.md`)](./docs/POST_MORTEM_V1.md)**: Why the initial QEMU + SPICE + Qt6 architecture encountered latency and integration bottlenecks.
- **[Architecture v2 Specification (`docs/ARCHITECTURE_V2.md`)](./docs/ARCHITECTURE_V2.md)**: Proposed system design; unverified sections remain design targets.
- **[Input & Graphics Pipeline (`docs/INPUT_AND_GRAPHICS_PIPELINE.md`)](./docs/INPUT_AND_GRAPHICS_PIPELINE.md)**: Proposed display/input architecture; only the raw input proof linked above is currently verified.
- **[Security research documents (`docs/STEALTH_AND_SECURITY.md`)](./docs/STEALTH_AND_SECURITY.md)**: Historical threat-model and owned-lab proposals, excluded from default release packaging and not validated as bypass capabilities.
- **[Native security research proposals (`docs/NATIVE_STEALTH_AND_ZYGISK_STRATEGY.md`)](./docs/NATIVE_STEALTH_AND_ZYGISK_STRATEGY.md)**: Historical research notes, not product claims or stable-channel scope.
- **[Commercial anti-cheat analysis (`docs/COMMERCIAL_ANTI_CHEAT_ANALYSIS.md`)](./docs/COMMERCIAL_ANTI_CHEAT_ANALYSIS.md)**: Threat-model notes; no compatibility or evasion claim.
- **[Hardware, Audio, Media & Multi-Instance Pipeline (`docs/HARDWARE_AUDIO_MEDIA_PIPELINE.md`)](./docs/HARDWARE_AUDIO_MEDIA_PIPELINE.md)**: Proposed subsystems awaiting runtime gates.
- **[Comprehensive Exposure Matrix (`docs/COMPREHENSIVE_ANTI_DETECTION_MATRIX.md`)](./docs/COMPREHENSIVE_ANTI_DETECTION_MATRIX.md)**: Research checklist; the executed defensive collector and its detectable result are in `proof/`.
- **[Evidence-Led Roadmap (`docs/ROADMAP.md`)](./docs/ROADMAP.md)**: Gate-based implementation and release milestones.

---

## 🗂️ Legacy Codebase

The initial experimental QEMU + SPICE + Qt6 implementation has been archived in the [`archive/v1-qemu-spice-qt`](https://github.com/eunhhu/RexPlayer/tree/archive/v1-qemu-spice-qt) branch for reference.

---

## 📄 License

RexPlayer is licensed under the [MIT License](./LICENSE).
All reverse engineering and analysis capabilities are intended strictly for educational and security research purposes.
