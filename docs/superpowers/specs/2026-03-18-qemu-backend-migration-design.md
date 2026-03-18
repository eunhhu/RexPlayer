# RexPlayer QEMU Backend Migration Design

**Date:** 2026-03-18
**Status:** Approved
**Scope:** Replace custom VMM with QEMU subprocess + SPICE display

## Problem

RexPlayer's custom VMM (HAL + device emulation) cannot run Android guests.
The device emulation layer is incomplete — no functional GIC, virtio-gpu, virtio-input, etc.
Building all of these from scratch is impractical. QEMU already provides a complete, battle-tested machine model.

## Decision

Replace the custom VMM with QEMU as a subprocess. RexPlayer becomes a **frontend** that:
1. Manages QEMU process lifecycle
2. Receives guest display via SPICE (low-latency)
3. Sends input events via SPICE input channel
4. Controls VM state via QMP (QEMU Machine Protocol)
5. Integrates Frida, config management, and auto-update via Rust middleware

## Architecture

```
┌─────────────────────────────────────┐
│  Qt 6 GUI (RexPlayer)              │
│  SPICE Client + VM Controls        │
├─────────────────────────────────────┤
│  QEMU Process Manager              │
│  spawn, monitor, kill, config gen  │
├─────────────────────────────────────┤
│  SPICE Display Channel             │
│  framebuffer → QImage rendering    │
│  input events → SPICE channel      │
├──── Rust FFI (cxx) ─────────────────┤
│  rex-config  │ rex-frida │ rex-update│
├─────────────────────────────────────┤
│  qemu-system-{aarch64,x86_64}      │
│  (subprocess)                       │
│  SPICE socket + QMP socket         │
└─────────────────────────────────────┘
```

## Components

### 1. QEMU Process Manager (`src/qemu/`)

**Responsibility:** Launch, monitor, and terminate QEMU subprocess.

**Files:**
- `qemu_process.h/.cpp` — QProcess wrapper, QEMU binary discovery, command-line generation
- `qemu_config.h/.cpp` — Translate RexPlayer config (CPU, RAM, disk, network) into QEMU CLI args
- `qmp_client.h/.cpp` — QMP (JSON over socket) client for VM control

**QEMU Binary Discovery (priority order):**
1. User-configured path in TOML config
2. Bundled binary (packaging/qemu/)
3. System PATH lookup (`qemu-system-aarch64`, `qemu-system-x86_64`)

**QEMU CLI Generation Example:**
```
qemu-system-aarch64 \
  -machine virt,gic-version=3 \
  -cpu cortex-a76 \
  -smp 4 \
  -m 4096 \
  -drive file=system.img,format=raw,if=virtio \
  -kernel Image \
  -append "console=ttyAMA0 androidboot.hardware=rex" \
  -spice unix=on,addr=/tmp/rex-spice.sock,disable-ticketing=on \
  -qmp unix:/tmp/rex-qmp.sock,server=on,wait=off \
  -display none \
  -device virtio-gpu-pci \
  -device virtio-keyboard-pci \
  -device virtio-tablet-pci \
  -device intel-hda -device hda-duplex \
  -netdev user,id=net0,hostfwd=tcp::5555-:5555 \
  -device virtio-net-pci,netdev=net0 \
  -serial mon:stdio
```

**Process Lifecycle:**
- `start()` — spawn QProcess, connect QMP, connect SPICE
- `pause()` — QMP `stop`
- `resume()` — QMP `cont`
- `reset()` — QMP `system_reset`
- `poweroff()` — QMP `system_powerdown`
- `kill()` — QMP `quit`, then QProcess::kill() if needed (5s timeout)
- `snapshot_save(name)` — QMP `snapshot-save` (QEMU 8.0+)
- `snapshot_load(name)` — QMP `snapshot-load` (QEMU 8.0+)

**Error Recovery:**
- **Crash detection:** `QProcess::finished` signal with non-zero exit code or crash exit status
- **Hang detection:** QMP heartbeat via `query-status` every 5s; if no response within 10s, treat as hung
- **Kill timeout:** QMP `quit` → wait 5s → `QProcess::kill()` → wait 2s → `QProcess::terminate()`
- **Socket cleanup:** On abnormal exit, delete stale SPICE/QMP unix sockets (Linux/macOS)
- **User notification:** Emit signal to GUI with error code and stderr output for display in status bar / dialog
- **No auto-restart:** Crashed VM requires explicit user action to restart (prevents boot loops)

### 2. QMP Client (`src/qemu/qmp_client.h/.cpp`)

**Responsibility:** JSON-RPC over unix socket (Linux/macOS) or named pipe (Windows).

**Protocol:**
- Connect to QMP socket
- Read greeting, send `qmp_capabilities`
- Send commands as JSON: `{"execute": "stop"}`
- Parse responses and async events

**Implementation:**
- `QLocalSocket` (Qt) for cross-platform unix socket / named pipe
- Async command queue with callbacks
- Event listener for guest state changes

### 3. SPICE Client (`src/spice/`)

**Responsibility:** Receive display frames and send input events via SPICE protocol.

**Files:**
- `spice_client.h/.cpp` — Session management, channel callbacks
- `spice_display.h/.cpp` — Display channel → QImage conversion
- `spice_input.h/.cpp` — Qt input events → SPICE input channel

**Library:** `libspice-client-glib` (C library, GLib dependency only — no GTK)

**Connection:**
- Linux/macOS: unix socket `-spice unix=on,addr=<unique-path>,disable-ticketing=on`
- Windows: TCP localhost `-spice port=<dynamic>,disable-ticketing=on`
- Socket paths include instance UUID to avoid collision when running multiple instances

**Display Pipeline:**
```
QEMU virtio-gpu → SPICE server → socket → libspice-client-glib
  → SpiceDisplayChannel invalidate callback
  → copy pixel data to QImage
  → QWidget::update()
```

**Input Pipeline:**
```
Qt QKeyEvent/QMouseEvent/QTouchEvent
  → SpiceInputsChannel
  → SPICE server → QEMU → guest virtio-input
```

**Event Loop Integration (GLib ↔ Qt):**

`libspice-client-glib` requires a GLib main context for async dispatch. Integration strategy:
- Use a dedicated `QTimer` (16ms interval ≈ 60fps) calling `g_main_context_iteration(ctx, FALSE)` to pump GLib events from the Qt thread
- This avoids threading complexity while maintaining low latency
- The GLib context is created per-SpiceSession, not the global default, to avoid conflicts
- If frame rate needs exceed 60fps, the timer interval can be reduced or switched to `QSocketNotifier` polling GLib file descriptors directly

### 4. Qt GUI Refactoring (`src/gui/`)

**Changes from current:**
- `MainWindow` — replace VM direct control with QemuProcess signals/slots
- `DisplayWidget` — replace software renderer with SPICE display sink
- `InputHandler` — route events to SPICE input channel instead of vCPU inject
- `SettingsDialog` — QEMU path config, display options
- `KeymapEditor` — retained, adapt to send keymaps via SPICE input channel
- `main.cpp` — remove HAL/VMM initialization, launch QemuProcess

**New GUI features:**
- QEMU binary path configuration
- Snapshot manager (save/load/delete via QMP)
- Power menu: pause, resume, reset, poweroff
- Status bar: QEMU process state, SPICE connection state

### 5. Rust Middleware (retained crates)

**rex-config** — TOML config management
- Add QEMU-specific config fields (binary path, machine type, accelerator)
- Generate QEMU CLI args from config

**rex-frida** — Frida server lifecycle
- Inject Frida into QEMU guest via ADB over forwarded port
- No changes needed to core logic

**rex-update** — Auto-update
- Add QEMU binary update path (optional bundled QEMU)
- Self-update mechanism unchanged

### 6. Deleted Middleware

- `rex-ffi` — C++↔Rust FFI bridge for custom VMM dispatch (entirely obsolete)
- `rex-devices` — All virtio device backends (blk, net, gpu, input, console, balloon, snd, vsock, mmio) — QEMU provides all devices
- `rex-network` — QEMU SLIRP handles networking
- `rex-filesync` — SPICE shared folder or virtio-9p replaces this

**Post-migration `Cargo.toml` workspace members:**
```toml
[workspace]
members = ["rex-config", "rex-frida", "rex-update"]
```

## Platform-Specific Notes

### macOS
- QEMU uses HVF accelerator: `-accel hvf`
- SPICE via unix socket
- QEMU installed via Homebrew (`brew install qemu`) or bundled
- **`libspice-client-glib` on macOS:**
  - Install via Homebrew: `brew install spice-gtk` (pulls GLib as dependency; GTK is a build dep but not linked at runtime if only using `libspice-client-glib`)
  - ARM64 (Apple Silicon): confirmed available in Homebrew arm64 bottles
  - Alternative: build `spice-gtk` from source with `-Dgtk=false` Meson option to eliminate GTK dependency entirely
  - GLib event loop integration handled via QTimer polling (see Event Loop Integration section)

### Linux
- QEMU uses KVM accelerator: `-accel kvm`
- SPICE via unix socket
- QEMU from system package manager

### Windows
- QEMU uses WHPX accelerator: `-accel whpx` (or TCG fallback)
- SPICE via TCP localhost (named pipes possible but less portable)
- QEMU from installer or bundled
- `libspice-client-glib` via vcpkg or pre-built

## Deleted Code

All of these directories are removed entirely:

- `src/hal/` — Hypervisor abstraction (KVM, HVF, WHPX direct calls)
- `src/vmm/` — Custom VM lifecycle, memory manager, boot, snapshot, optimizers
- `src/devices/` — Legacy device emulation (UART, i8042, RTC, PCI)
- `src/gpu/` — Software renderer, virgl/venus stubs
- `src/platform/` — OS abstraction, async I/O
- `middleware/rex-ffi/` — C++↔Rust FFI bridge (obsolete with QEMU)
- `middleware/rex-devices/` — All virtio device backends (QEMU replaces)
- `middleware/rex-network/`
- `middleware/rex-filesync/`

## Dependencies

### New
- `libspice-client-glib` — SPICE protocol client
- `glib-2.0` — Required by spice-client-glib

### Retained
- Qt 6 (Widgets, Gui, Core)
- Rust toolchain (for middleware crates)
- cxx (Rust FFI bridge)

### Removed
- Hypervisor.framework (macOS)
- Direct KVM/WHPX headers
- virglrenderer, Venus

## CMake Structure (new)

```cmake
# Core libraries
add_library(rex_qemu STATIC
    src/qemu/qemu_process.cpp
    src/qemu/qemu_config.cpp
    src/qemu/qmp_client.cpp
)

add_library(rex_spice STATIC
    src/spice/spice_client.cpp
    src/spice/spice_display.cpp
    src/spice/spice_input.cpp
)

# GUI executable
add_executable(rexplayer
    src/gui/main.cpp
    src/gui/mainwindow.cpp
    src/gui/display_widget.cpp
    src/gui/input_handler.cpp
    src/gui/settings_dialog.cpp
    src/gui/keymap_editor.cpp
)
target_link_libraries(rexplayer PRIVATE
    rex_qemu rex_spice
    Qt6::Widgets Qt6::Gui Qt6::Core
    PkgConfig::SPICE_CLIENT_GLIB
)
```

## Testing Strategy

- **QMP Client**: Unit tests with mock socket (JSON request/response pairs)
- **QEMU Config**: Unit tests for CLI arg generation from various configs
- **SPICE Display**: Integration test with QEMU headless + frame capture
- **GUI**: Manual testing for now; consider Qt Test framework later
- **Rust crates**: Existing tests for rex-config, rex-frida, rex-update

## Migration Steps (high level)

1. Delete old code (HAL, VMM, devices, GPU, platform)
2. Delete unused Rust crates (rex-network, rex-filesync)
3. Implement QEMU process manager + QMP client
4. Implement SPICE client (display + input)
5. Refactor GUI to use new backends
6. Update CMakeLists.txt
7. Update rex-config for QEMU settings
8. Integration test: boot Android on all 3 platforms
9. Update README and docs
