# RexPlayer v2: Development Roadmap & Milestones

This roadmap outlines the phased development strategy for **RexPlayer v2**. Each milestone defines clear engineering deliverables, verification gates, and architectural targets.

---

## 🗺️ High-Level Phase Overview

```
[ Phase 0: Substrate & Kernel PoC ] ──▶ [ Phase 1: Input & Container Bridge ] ──▶ [ Phase 2: GPUI Host Shell & HUD ]
                                                                                               │
[ Phase 4: CI/CD & Automated Distribution ] ◀── [ Phase 3: Stealth & Frida Analysis Studio ] ◄─┘
```

---

## 📌 Phase 0: Substrate & Kernel PoC (Weeks 1–2)

**Goal:** Establish the foundational container virtualization layer and build the custom kernel for WSL2 and Native Linux.

- [ ] **WSL2 Custom Kernel Pipeline:**
  - Build automated GitHub Actions workflow to compile `microsoft/WSL2-Linux-Kernel` with `CONFIG_ANDROID_BINDER_IPC=y`, `CONFIG_ANDROID_BINDERFS=y`, `CONFIG_ASHMEM=y`, and **KernelSU** tree integration.
  - Test kernel injection on Windows 11 host via `.wslconfig`.
- [ ] **Waydroid Container & Network Bootstrap:**
  - Build automated initialization script for Waydroid inside WSL2 / Ubuntu.
  - Implement automated `iptables` NAT forwarding rules to ensure 100% stable internet & Google Play connectivity.
  - Integrate `libndk` / `libhoudini` ARM64 translation layer and verify 3D game execution (e.g. Unity / Unreal APK).

---

## 📌 Phase 1: High-Performance Input & Control Bridge (Weeks 3–4)

**Goal:** Implement the zero-latency input translation engine mapping host keyboard/mouse to Android multi-touch.

- [ ] **Rust Input Daemon (`rex-input-bridge`):**
  - Implement `/dev/uinput` virtual multi-touch device emulator inside the Linux container.
  - Implement low-level host keyboard hook (Windows RawInput / Linux libevdev).
  - Implement directional vector math for virtual WASD joystick emulation.
  - Implement Free-Aim FPS mouse trapping and continuous touch swipe engine.
- [ ] **Socket IPC:**
  - Ultra-low latency binary IPC protocol (Unix socket / Named Pipe) connecting host input hooks directly to the container's `/dev/uinput`.

---

## 📌 Phase 2: GPUI Native Host Shell & Direct GPU HUD (Weeks 5–6)

**Goal:** Build the ultra-lightweight, 120Hz/240Hz GPU-accelerated desktop frontend and transparent HUD using GPUI (Pure Rust).

- [ ] **GPUI Native Shell (`rex-gui`):**
  - High-performance desktop UI written 100% in Rust using GPUI (direct Metal/Vulkan/Direct3D 12 rendering, <30MB RAM footprint).
  - Process lifecycle manager (Launch, Pause, Restart, Kill Android session).
- [ ] **Zero-Copy Viewport Compositing (`rex-window-dock`):**
  - Ingest Android render surface via direct GPU texture handles (WSLg D3D12 shared resource, Wayland DMA-BUF, macOS `IOSurface`).
  - Seamlessly dock sidebars, floating palettes, and game controls to the viewport bounds.
- [ ] **Visual Keymapper & Hardware-Accelerated Transparent HUD:**
  - Zero-latency transparent overlay rendered directly over the game viewport via custom GPUI quad shaders.
  - Visual touch-point animations, keybind highlights, and FPS aim crosshairs.
  - Save, export, and load per-game keymap profile presets (`.toml`).

---

## 📌 Phase 3: Stealth & Security Analysis Studio (Weeks 7–8)

**Goal:** Implement anti-cheat evasion, kernel-level hooks, and the integrated Frida script studio.

- [ ] **Stealth Kernel Hooks:**
  - Kernel-level `TracerPid` zeroing and anti-ptrace bypass in `fs/proc/array.c`.
  - Spoof hardware build properties and eliminate emulator device tree artifacts.
- [ ] **Frida Studio (Native High-Speed Code Editor):**
  - High-performance code editor built with GPUI and Tree-sitter for TypeScript/JavaScript Frida scripts.
  - Covert Frida runner: randomized thread names, hidden ports, and abstract socket delivery.
  - One-click memory dumper for Unity IL2CPP games (`global-metadata.dat` + decrypted `.so` dump).

---

## 📌 Phase 4: Automated Packaging & Distribution (Weeks 9–10)

**Goal:** Deliver a seamless one-click installer experience for Windows and Linux.

- [ ] **Windows One-Click Installer (NSIS / Rust Bundle):**
  - Automatically provisions WSL2, installs the custom kernel, extracts the rootfs, and launches RexPlayer.
- [ ] **Linux Package (AppImage / DEB / Arch PKGBUILD):**
  - Single-command setup for Linux distributions.
- [ ] **Automated GitHub Release CI/CD:**
  - Automated binary builds, kernel compilation, and OTA update channel (`rex-update`).

---

## 🎯 Verification Gates

1. **Gate 1 (Gaming):** Run a high-end 3D mobile game at a stable 120 FPS with fluid WASD and mouse-aim controls on Windows 11.
2. **Gate 2 (Stealth):** Launch a top-tier anti-cheat protected game with Frida attached and verify zero detection/bans.
3. **Gate 3 (Resource Footprint):** Verify idle RAM usage stays strictly below 600 MB.
