# Input & Graphics Pipeline Specification

This document details the low-level graphics rendering and input translation architecture of **RexPlayer v2**, explaining how near-zero display latency and microsecond-level touch response are achieved across Windows and Linux.

---

## 1. Graphics Rendering Architecture (Zero-Copy Pipeline)

In traditional emulators, passing graphic frames between the guest OS and the host GUI requires memory copies or network serialization (as seen in RexPlayer v1's SPICE pipeline). RexPlayer v2 completely eliminates intermediate copies.

### 1.1 Windows Pipeline (Direct3D 12 via WSLg)

```
[ Android Guest App (e.g. Unity / Unreal Engine) ]
  ├── Calls GLES / Vulkan APIs
  └── Waydroid SurfaceFlinger composites frames
        │
        ▼ (Mesa d3d12 Gallium Driver inside WSL2)
  [ /dev/dxg Kernel Driver (Hyper-V D3D12 Passthrough) ]
        │
        ▼ (Direct Submission to Windows Host WDDM)
  [ Host Physical GPU (NVIDIA / AMD / Intel) ]
        │
        ▼ (WSLg RDP/RAIL Zero-Copy Presentation)
  [ Win32 Native Render Surface (HWND) / Shared D3D12 Texture ]
        │
        ▼
  [ RexPlayer GPUI Host Shell (Direct GPU Accelerated Viewport) ]
```

- **Frame Latency:** < 2 ms.
- **Max Refresh Rate:** Supports 60 Hz, 120 Hz, 144 Hz, and 240 Hz natively matching the host monitor.
- **Resource Utilization:** 0% CPU software rasterization overhead.

### 1.2 Linux Pipeline (Mesa DRI3 / DMA-BUF / Wayland)

- Waydroid talks directly to the host Wayland compositor using standard Linux DMA-BUF memory buffers.
- GPUI imports DMA-BUF surfaces directly via WGPU/Vulkan texture descriptors, eliminating all host-guest blitting overhead.

### 1.3 macOS / Darwin Pipeline (Metal & IOSurface)

- Guest graphic buffers in virtualized containers are exported directly to macOS `IOSurface` objects.
- GPUI's Metal backend binds the `IOSurfaceRef` as a native Metal texture (`id<MTLTexture>`) within its render loop, providing pure 120Hz ProMotion display rendering with zero host memory copies.

---

## 2. Low-Latency Input Translation Pipeline

### 2.1 The Multi-Touch Problem
Mobile games are designed for capacitive multi-touch screens. To map keyboard keys (`W`, `A`, `S`, `D`, `Space`, etc.) and mouse motions to in-game actions, RexPlayer implements a high-precision input bridge powered by Linux's `/dev/uinput`.

```
┌─────────────────────────────────────────────────────────────┐
│ 1. Host Input Capture                                       │
│    • Windows: Low-Level Keyboard & Mouse Hooks (RawInput)   │
│    • Linux: libevdev / X11 / Wayland Global Input Grab      │
└──────────────────────────────┬──────────────────────────────┘
                               │ (Zero-allocation Ring Buffer)
┌──────────────────────────────▼──────────────────────────────┐
│ 2. Rex-Input Translation Engine (Rust)                      │
│    • Resolves Keymap JSON AST:                              │
│      - Key Press ──▶ Multi-touch Point Down (x, y)          │
│      - Key Hold (WASD) ──▶ Vector Math (Virtual Joystick)   │
│      - Mouse Delta (dx, dy) ──▶ Delta Swipe (FPS Aim)       │
│      - Skill Cast Button ──▶ Touch Slide & Release          │
└──────────────────────────────┬──────────────────────────────┘
                               │ (Microsecond Socket Transport)
┌──────────────────────────────▼──────────────────────────────┐
│ 3. Linux Kernel /dev/uinput Virtual Touchscreen             │
│    • Emulates ABS_MT_SLOT, ABS_MT_TRACKING_ID               │
│    • Emulates ABS_MT_POSITION_X, ABS_MT_POSITION_Y         │
│    • SYN_REPORT packet dispatch                             │
└──────────────────────────────┬──────────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────────┐
│ 4. Android InputManager                                     │
│    • Delivers standard android.view.MotionEvent             │
│    • Target game detects genuine multi-touch interaction    │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Keymap Types & Mathematics

### 3.1 Virtual Joystick (WASD Movement)
When the user presses directional keys, the engine calculates smooth radial coordinates rather than instant jumps to prevent anti-cheat movement speed flagging:

$$\vec{V} = (k_D - k_A) \cdot \hat{x} + (k_S - k_W) \cdot \hat{y}$$

$$\text{Point}(t) = \text{Center} + R \cdot \frac{\vec{V}}{\|\vec{V}\|} \cdot (1 - e^{-t/\tau})$$

### 3.2 FPS Mouse-Look (Free Aim Mode)
- **Mouse Trapping:** When Free Aim mode is toggled (e.g. via `~` or `Right-Click`), the host cursor is locked and hidden.
- **Continuous Touch Swipe:** As the mouse moves by $(\Delta x, \Delta y)$, the engine injects a continuous drag vector on the camera area. When the touch point approaches the screen boundary, the engine seamlessly resets the tracking slot with an instantaneous `ACTION_UP` and `ACTION_DOWN` at the origin without dropping camera momentum.

### 3.3 Skill Directional Casting (Smart Cast)
- **Down Event:** Pressing skill key (e.g. `Q`) touches the skill button origin.
- **Move Vector:** Moving the mouse while holding `Q` drags the touch point radially from the origin to aim the skill.
- **Release Event:** Releasing `Q` sends `ACTION_UP`, instantly firing the skill in the desired direction.

---

## 4. UI Docking & Transparent Overlay

```
┌──────────────────────────────────────────────────────────────┐
│ [Android Render Surface (HWND / Metal / Vulkan Texture)]     │
│                                                              │
│  (Transparent GPUI Hardware HUD Overlay - On Edit / Live)    │
│   ┌──────┐         ┌──────┐                                  │
│   │ [W]  │         │ [Q]  │ ── Drag-and-drop skill buttons   │
│ ┌─┴──────┴─┐       └──────┘                                  │
│ │[A] [S] [D]│      ┌──────┐                                  │
│ └──────────┘       │[SPC] │                                  │
│                    └──────┘                                  │
│                                                              │
├──────────────────────────────────────────────────────────────┤
│ [GPUI Cyber-Dock Sidebar]                                    │
│  [🎮 Keymap] [⚙️ Settings] [💉 Frida Studio] [📷 Screenshot] │
└──────────────────────────────────────────────────────────────┘
```

1. **Overlay Synchronization:**
   - The keymap editor and live analysis HUD run inside a transparent GPUI window rendered directly above the Android viewport via custom GPU shader pipelines.
   - When keymapping is completed, the overlay becomes transparent and click-through (`WS_EX_TRANSPARENT` on Windows, Wayland input passthrough on Linux).
2. **Dynamic DPI Scaling:**
   - Automatically computes resolution scale factors between host monitor DPI and Android's internal `ro.sf.lcd_density` to guarantee pixel-perfect touch coordinates.
