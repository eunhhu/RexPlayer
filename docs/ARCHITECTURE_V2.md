# RexPlayer v2: Architecture & System Specification

RexPlayer v2 is a container-native Android execution environment and security analysis platform designed for Windows 11 (via WSL2) and Native Linux. This document specifies the comprehensive technical architecture, component boundaries, IPC protocols, and execution lifecycles.

---

## 1. System Topology & Component Matrix

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│                               TAURI v2 FRONTEND                                  │
│   • Svelte 5 / TailwindCSS / Lucide Icons / Framer-Motion (Cyber Glass Aesthetic)│
│   • Floating Action Dock / Sidebar Controller                                    │
│   • Visual Touch Keymapper & Mouse Sensitivity Tuner                             │
│   • Monaco Code Studio (Frida JavaScript / TypeScript Editor & Console)          │
└────────────────────────────────────────┬─────────────────────────────────────────┘
                                         │ (Tauri IPC / Rust Commands & Events)
┌────────────────────────────────────────▼─────────────────────────────────────────┐
│                           REX-CORE (Rust Native Engine)                          │
│                                                                                  │
│   ┌────────────────────────┐  ┌────────────────────────┐  ┌───────────────────┐  │
│   │   rex-input-bridge     │  │      rex-container     │  │   rex-security    │  │
│   │  • Win32 RawInput      │  │  • WSL2 Engine (Win)   │  │  • KernelSU IPC   │  │
│   │  • Linux evdev Hook    │  │  • LXC/Waydroid (Linux)│  │  • Covert Frida   │  │
│   │  • Keymap Config / AST │  │  • Custom Kernel Deploy│  │  • Syscall Spoof  │  │
│   │  • Virtual /dev/uinput │  │  • IP/GApps Network Fix│  │  • Memory Dump    │  │
│   └────────────────────────┘  └────────────────────────┘  └───────────────────┘  │
│                                                                                  │
│   ┌───────────────────────────────────────────────────────────────────────────┐  │
│   │   rex-window-dock (Window Embedding & Subsurface Tracking)                │  │
│   │   • Tracks Win32 HWND (WSLg) / Linux Wayland wl_surface                   │  │
│   │   • Pins Sidebar & Overlays seamlessly to the Android render surface      │  │
│   └───────────────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────┬─────────────────────────────────────────┘
                                         │
                   ┌─────────────────────┴─────────────────────┐
                   │                                           │
    [ Windows 11 (WSL2 Host) ]                     [ Linux Native Host ]
                   │                                           │
┌──────────────────▼───────────────────┐     ┌─────────────────▼───────────────────┐
│ • Microsoft Hyper-V Micro-VM         │     │ • Host Linux Kernel 6.x             │
│ • Custom Kernel (KernelSU + BinderFS)│     │ • BinderFS & Ashmem Native Modules  │
│ • WSLg Direct3D 12 (/dev/dxg)        │     │ • Mesa DRI3 / DMA-BUF GPU Bridge    │
│ • Isolated Bridge Network            │     │ • Wayland Compositor Session        │
└──────────────────┬───────────────────┘     └─────────────────┬───────────────────┘
                   │                                           │
                   └─────────────────────┬─────────────────────┘
                                         ▼
┌──────────────────────────────────────────────────────────────────────────────────┐
│                      WAYDROID CONTAINER RUNTIME (AOSP 13/14)                     │
│                                                                                  │
│   • SurfaceFlinger ──▶ Zero-Copy DMA-BUF ──▶ Host Display Compositor             │
│   • Android InputManager ──▶ Reads `/dev/input/event*` created by RexInput      │
│   • ARM64 Binary Translation (libndk / libhoudini JIT Engine)                    │
│   • KernelSU Core Daemon (Direct Kernel Hooking, zero userspace trace)           │
│   • Spoofed System Properties (`ro.product.model=Pixel 8 Pro`, pure DRM)         │
└──────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Core Subsystems

### 2.1 The Container & Kernel Orchestrator (`rex-container`)

RexPlayer manages the full lifecycle of the containerized Android runtime without requiring manual user intervention:

#### Windows Automation (WSL2 Engine):
1. **Kernel Injection:**
   - On initial launch, `rex-container` checks `%USERPROFILE%\.wslconfig`.
   - If RexPlayer's custom kernel (`vmlinux-rex-kernelsu`) is not registered, it places the precompiled binary in `%USERPROFILE%\.rexplayer\kernel\` and injects:
     ```ini
     [wsl2]
     kernel=%USERPROFILE%\\.rexplayer\\kernel\\vmlinux-rex-kernelsu
     memory=4GB
     processors=4
     nestedVirtualization=true
     ```
2. **Rootfs Provisioning:**
   - Ingests a pre-configured, lightweight Debian/Ubuntu rootfs containing Waydroid and libndk dependencies via `wsl --import RexPlayer-Engine <Path> <RootfsArchive>`.
3. **Automated Network & GApps Patching:**
   - Automatically injects iptables NAT rules inside WSL2 to prevent DNS/gateway drops:
     ```bash
     iptables -A FORWARD -i waydroid0 -j ACCEPT
     iptables -A FORWARD -o waydroid0 -j ACCEPT
     iptables -t nat -A POSTROUTING -s 192.168.240.0/24 -j MASQUERADE
     ```

#### Linux Automation:
- Verifies `binderfs` mount point at `/dev/binderfs`.
- Manages `waydroid-container.service` lifecycle over DBus/Systemd.

---

### 2.2 Input Translation Subsystem (`rex-input-bridge`)

To deliver competitive gaming responsiveness, RexPlayer intercepts host hardware input and translates it into virtual multi-touch points via the Linux kernel's `uinput` interface:

```
[ Host Input Event ] (e.g., Key 'W' pressed, Mouse moved)
        │
        ▼ (Low-Level Windows RawInput / Linux libevdev)
[ Rex-Core Input Processor ]
        │  • Looks up active Game Profile (Keymap AST)
        │  • Computes Virtual Joystick / Skill Cast coordinates
        │  • Interpolates Mouse Aim (FPS Free-Look Mode)
        ▼
[ Linux /dev/uinput Driver ]
        │  • Emulates `/dev/input/eventX` (Multi-Touch ABS_MT protocol)
        ▼
[ Android InputReader / InputDispatcher ]
        │  • Dispatches MotionEvent (ACTION_DOWN, ACTION_MOVE, ACTION_POINTER_DOWN)
        ▼
[ Game Touch Listener ] (Zero perceived lag, true hardware multi-touch)
```

#### Key Capabilities:
- **Virtual D-Pad / Joystick:** Translates `WASD` into fluid circular touch motions with configurable deadzones.
- **FPS Mouse Lock:** Traps the host mouse cursor, converts raw delta movements (`dx, dy`) into continuous swipe gestures on the camera region.
- **Multi-Touch Concurrency:** Supports up to 10 simultaneous virtual touch points without dropped packets.

---

### 2.3 Window Docking & Display Compositor (`rex-window-dock`)

RexPlayer bypasses slow frame capture by utilizing **Subsurface Window Docking**:

1. **Zero-Copy Display:**
   - On Windows, WSLg renders the Waydroid surface directly via Direct3D 12 and presents it in a native Win32 window.
   - On Linux, Waydroid renders directly onto the host Wayland/X11 surface via Mesa DRI3.
2. **Tauri Docking Mechanics:**
   - Tauri monitors the position and dimensions of the Android render window (`HWND` on Windows, `wl_surface` on Linux).
   - The Tauri UI automatically docks its sidebars, controls, and floating toolbars to the edges of the Android window, creating the illusion of a unified, custom application.
3. **Transparent Keymap Overlay:**
   - When editing keymaps, Tauri opens a transparent, click-through overlay window positioned exactly over the Android canvas.

---

### 2.4 Security & Reverse Engineering Subsystem (`rex-security`)

```
[ Tauri Security Studio (Monaco Editor) ]
                   │
                   ▼ (WebSocket / IPC Channel)
[ Rex-Security Daemon (Host Rust Process) ]
                   │
                   ▼ (Abstract Unix Domain Socket - Stealth Channel)
[ Covert Frida Daemon (Guest Android Container) ]
                   │
                   ▼ (Direct Memory Injection)
[ Target Mobile Game Process ] ──▶ Anti-Cheat Bypass:
                                    • TracerPid spoofed to 0
                                    • Maps clean (no Frida gadgets)
                                    • Ports clean (no 27042 bound)
```

#### Security Capabilities:
- **KernelSU Integration:** Root access granted exclusively via direct kernel system call checks without creating any `su` binary artifacts on disk.
- **Covert Frida:** Runs custom patched Frida binaries with randomized thread signatures, cloaked memory regions, and abstract socket endpoints.
- **Automated Memory Dumper:** Integrated Dex/Il2Cpp memory dumper that extracts metadata and decrypted `.so` libraries directly upon process execution.
