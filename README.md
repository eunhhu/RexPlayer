# RexPlayer v2: Next-Gen Ultra-Lightweight Android Engine & Security Analysis Platform

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%2011%20%7C%20Linux-blue.svg)](#supported-platforms)
[![UI](https://img.shields.io/badge/Frontend-Tauri%20v2%20%2B%20Svelte-FF3E00.svg)](#frontend--ui)
[![Engine](https://img.shields.io/badge/Engine-WSL2%20%2F%20Waydroid%20(LXC)-2496ED.svg)](#engine--virtualization)
[![Stealth](https://img.shields.io/badge/Stealth-KernelSU%20%2B%20Covert%20Frida-black.svg)](#stealth--security-analysis)

> **RexPlayer v2** is a revolutionary, container-native Android execution and security analysis platform for Windows and Linux.
> By leveraging **LXC / Waydroid inside WSL2 (Windows) and Native Linux** with a **Custom Kernel (KernelSU + BinderFS)** and a **High-Performance Input/Graphics Bridge**, RexPlayer delivers near-native (0-copy) gaming performance, multi-touch mapping, zero bloatware, and undetectable reverse-engineering capabilities.

---

## 🧭 Why RexPlayer v2?

Traditional Android emulators (BlueStacks, Nox, LDPlayer, MuMu) rely on heavy, outdated VirtualBox/QEMU hypervisors bundled with adware, mining bloat, and easily detected emulator artifacts. Meanwhile, stock AVD is too slow for 3D workloads, and Microsoft WSA was abandoned due to app ecosystem constraints.

**RexPlayer v2 breaks the paradigm:**

```
Traditional Emulators (Type-2 VM)             RexPlayer v2 (Container + Direct Acceleration)
┌──────────────────────────────────────┐     ┌──────────────────────────────────────┐
│  Host OS (Win/Linux)                 │     │  Host OS (Windows 11 / Linux)        │
│  └── Heavy Hypervisor (QEMU/VBox)    │     │  └── Lightweight VM / Container Host │
│      └── Memory Overhead (>2GB idle) │     │      ├── Direct3D 12 / Mesa DRI3 GPU │
│      └── Virtual Devices (Detectable)│     │      ├── Host-shared RAM (Zero Copy) │
│      └── Adware & telemetry services │     │      └── Waydroid Container (AOSP)  │
│  [FPS: Stuttery / Ram: Heavy / AntiCheat: ❌]│     │  [FPS: 120+ Native / Ram: 400MB / Stealth: 🛡️]│
└──────────────────────────────────────┘     └──────────────────────────────────────┘
```

---

## ⚡ Key Architectural Highlights

| Feature | RexPlayer v1 (Legacy) | Commercial Players (LD/MuMu) | **RexPlayer v2** |
| :--- | :--- | :--- | :--- |
| **Virtualization Model** | QEMU VM + SPICE | Customized VirtualBox | **WSL2 / LXC Container (Zero VM Overhead)** |
| **Display Pipeline** | SPICE TCP Socket (Laggy) | Host GLES Hooking (Proprietary) | **WSLg D3D12 / Wayland Zero-Copy Direct Rendering** |
| **GUI Framework** | Heavy C++ / Qt 6 | Custom Win32 / Ad-heavy UI | **Ultra-Sleek Tauri v2 (Rust + Svelte + Cyber Glass)** |
| **Input Engine** | Basic SPICE Input | Proprietary Keymapping | **`/dev/uinput` Real Multi-touch + Low-Latency Mouse Lock** |
| **Root & Instrumentation** | Traditional `su` / Frida port | Settings Root Toggle (Detected) | **Kernel-Level KernelSU + Covert Frida IPC** |
| **Idle Memory (RAM)** | ~2.5 GB | ~1.8 GB - 3.0 GB | **~400 MB - 600 MB** |
| **Telemetry & Ads** | None | Extremely Heavy | **100% Zero Bloat / Open Source** |

---

## 🏛️ System Architecture

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                               TAURI v2 FRONTEND & DOCKING GUI                          │
│   • Cyber-Aesthetic Glassmorphism Interface (Svelte + Tailwind)                        │
│   • Native Window Docking (HWND / wl_surface tracker)                                  │
│   • Visual Keymap / Macro Editor & Transparent Overlay                                 │
│   • Built-in Monaco Frida Script Studio & Dynamic Memory Inspector                     │
└───────────────────────────────────────────┬────────────────────────────────────────────┘
                                            │ (Ultra-low latency Rust IPC / Channels)
┌───────────────────────────────────────────▼────────────────────────────────────────────┐
│                                   REX-CORE (Rust Native Engine)                        │
│   ┌───────────────────────────┬────────────────────────────┬────────────────────────┐  │
│   │ Input Translation Bridge  │ Stealth Lifecycle Manager  │ Frida Covert Controller│  │
│   │ RawInput/evdev ──▶ uinput │ Device Spoofing / Syscall  │ Unix Domain Sockets    │  │
│   └───────────────────────────┴────────────────────────────┴────────────────────────┘  │
└───────────────────────────────────────────┬────────────────────────────────────────────┘
                                            │
                     ┌──────────────────────┴──────────────────────┐
                     │                                             │
      [ Windows 11 (WSL2 Engine) ]                      [ Linux Native (LXC Engine) ]
                     │                                             │
┌────────────────────▼──────────────────────┐ ┌────────────────────▼──────────────────────┐
│  • Custom WSL2 Kernel (KernelSU + Binder) │ │  • Host Linux Kernel (KernelSU / eBPF)   │
│  • WSLg / Direct3D 12 GPU Passthrough     │ │  • Mesa DRI3 / DMA-BUF GPU Acceleration  │
│  • Automated wslconfig & Rootfs Ingestion │ │  • Native Wayland Compositor Session     │
└────────────────────┬──────────────────────┘ └────────────────────┬──────────────────────┘
                     │                                             │
                     └──────────────────────┬──────────────────────┘
                                            ▼
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                           WAYDROID CONTAINER (AOSP 13/14 GUEST)                        │
│   • ARM64/ARMv8 Translation Bridge (libndk / libhoudini)                               │
│   • KernelSU Embedded Root (No files in /system, Invisible to Anti-Cheat)              │
│   • Pure Android Runtime (ART) with Spoofed Hardware Fingerprints                      │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 🛡️ Anti-Cheat & Stealth Analysis Engine

RexPlayer v2 is engineered from the ground up for **game security research, CTF wargames, and reverse engineering**:

1. **Kernel-Level Root (KernelSU):**
   - No `su` binary in `/system/bin` or `/data/local/tmp`.
   - Superuser access is granted directly via customized kernel hooks when authorized.
2. **Anti-Debugging Countermeasures:**
   - `/proc/self/status` `TracerPid` spoofed to `0` at the kernel layer.
   - `ptrace(PTRACE_TRACEME)` anti-debugging bypass.
3. **Covert Instrumentation:**
   - Patched Frida server (no `27042` port signature, randomized process names, communication over abstract Unix Domain Sockets).
4. **Hardware Spoofing:**
   - Pure real-device fingerprints (`ro.product.model=Pixel 8 Pro`, genuine DRM L1/L3 mock, genuine OpenGL/Vulkan GL_RENDERER strings).

---

## 📚 Technical Documentation

Explore the detailed architecture and technical specifications in [`/docs`](./docs):

- **[Technical Precedents & Prior Art (`docs/TECHNICAL_PRECEDENTS.md`)](./docs/TECHNICAL_PRECEDENTS.md)**: Deep analysis of WSA, LDPlayer, MuMu, BlueStacks, Waydroid, ReDroid, and why prior attempts struggled.
- **[RexPlayer v1 Post-Mortem (`docs/POST_MORTEM_V1.md`)](./docs/POST_MORTEM_V1.md)**: Why the initial QEMU + SPICE + Qt6 architecture encountered fatal latency bottlenecks.
- **[Architecture v2 Specification (`docs/ARCHITECTURE_V2.md`)](./docs/ARCHITECTURE_V2.md)**: Complete system design, lifecycle, IPC protocols, and container orchestrator.
- **[Input & Graphics Pipeline (`docs/INPUT_AND_GRAPHICS_PIPELINE.md`)](./docs/INPUT_AND_GRAPHICS_PIPELINE.md)**: Zero-copy rendering, Direct3D 12/Mesa bridges, and `/dev/uinput` multi-touch translation.
- **[Stealth & Security Guide (`docs/STEALTH_AND_SECURITY.md`)](./docs/STEALTH_AND_SECURITY.md)**: In-depth guide on anti-cheat evasion, KernelSU integration, and Frida stealth hooks.
- **[Native Stealth & Zygisk Evasion Strategy (`docs/NATIVE_STEALTH_AND_ZYGISK_STRATEGY.md`)](./docs/NATIVE_STEALTH_AND_ZYGISK_STRATEGY.md)**: Strategies for natively baking in Magisk, Zygisk, Shamiko, and HMA evasion at the Kernel, Bionic libc, and AOSP layers.
- **[Commercial Anti-Cheat In-Depth Analysis (`docs/COMMERCIAL_ANTI_CHEAT_ANALYSIS.md`)](./docs/COMMERCIAL_ANTI_CHEAT_ANALYSIS.md)**: Analysis of LIAPP, XIGNCODE3 Mobile, Tencent ACE, and AppIron detection vectors and substrate-level defenses.
- **[Hardware, Audio, Media & Multi-Instance Pipeline (`docs/HARDWARE_AUDIO_MEDIA_PIPELINE.md`)](./docs/HARDWARE_AUDIO_MEDIA_PIPELINE.md)**: Low-latency PipeWire/WASAPI audio, hardware video decoding, 16KB memory page compatibility, and per-instance proxy routing.
- **[Comprehensive Anti-Detection Matrix (`docs/COMPREHENSIVE_ANTI_DETECTION_MATRIX.md`)](./docs/COMPREHENSIVE_ANTI_DETECTION_MATRIX.md)**: 50+ detection vector checklist covering GPU strings, CPU topology, storage block devices, sensor jitter, and Play Integrity.
- **[Development Roadmap (`docs/ROADMAP.md`)](./docs/ROADMAP.md)**: Phase-by-phase implementation plan and milestones.

---

## 🗂️ Legacy Codebase

The initial experimental QEMU + SPICE + Qt6 implementation has been archived in the [`archive/v1-qemu-spice-qt`](https://github.com/eunhhu/RexPlayer/tree/archive/v1-qemu-spice-qt) branch for reference.

---

## 📄 License

RexPlayer is licensed under the [MIT License](./LICENSE).
All reverse engineering and analysis capabilities are intended strictly for educational and security research purposes.
