# RexPlayer Architecture

RexPlayer is a native Android player that manages a QEMU subprocess for VM execution, connects to it via the SPICE protocol for display and input, and controls it via QMP (QEMU Machine Protocol). A Qt 6 GUI provides the user interface including a game keymap editor, Frida script editor, and modern settings dialog.

## Component Overview

```
┌────────────────────────────────────────────────────────────────┐
│                        Qt 6 GUI Layer                          │
│                                                                │
│  MainWindow            DisplayWidget       SettingsDialog      │
│  - Sidebar action      - SPICE surface     - VM config         │
│    buttons             - Touch/keyboard    - Display           │
│  - VM lifecycle          input forwarding  - Network           │
│    controls                                - Frida             │
│                        KeymapEditor                            │
│  FridaPanel            - Visual key        InputHandler        │
│  - Script editor         rebinding         - Qt event →        │
│  - Console output      - Game profiles       SPICE input       │
├────────────────────────────────────────────────────────────────┤
│                     SPICE Client Layer                         │
│                                                                │
│  SpiceClient           SpiceDisplay        SpiceInput          │
│  - Session setup       - Display channel   - Input channel     │
│  - Channel mgmt        - Frame rendering   - Key/mouse/touch   │
│  - Reconnect logic     - Resize handling     injection         │
├────────────────────────────────────────────────────────────────┤
│                      QMP Client Layer                          │
│                                                                │
│  QmpClient                                                     │
│  - Unix/TCP socket connection to QEMU                          │
│  - JSON command/response framing                               │
│  - Commands: query-status, stop, cont, system_reset,           │
│              savevm, loadvm, quit                              │
├────────────────────────────────────────────────────────────────┤
│                  QEMU Process Manager                          │
│                                                                │
│  QemuProcess           QemuConfig                             │
│  - QProcess lifecycle  - CLI argument assembly                 │
│  - stdout/stderr log   - Machine type, CPU, RAM                │
│  - Exit monitoring     - Drive/kernel/initrd args              │
│  - SPICE port alloc    - SPICE + QMP socket args               │
├────────────────────────────────────────────────────────────────┤
│                   QEMU Subprocess                              │
│                                                                │
│  macOS:   -accel hvf    (Hypervisor.framework)                 │
│  Linux:   -accel kvm    (/dev/kvm)                             │
│  Windows: -accel whpx   (Windows Hypervisor Platform)          │
└────────────────────────────────────────────────────────────────┘

Rust Middleware (separate processes / out-of-process)
  rex-config  — TOML config read/write
  rex-frida   — Frida server lifecycle inside the guest
  rex-update  — OTA self-update for the host binary
```

## Data Flow

### Display Pipeline

```
QEMU guest GPU driver
        │
        │  virtio-gpu or VGA framebuffer (inside QEMU)
        ▼
  QEMU SPICE server
        │
        │  SPICE protocol (Unix socket or TCP)
        ▼
  SpiceClient (spice-gtk session)
        │
        ▼
  SpiceDisplay (display channel)
        │  decoded frame / damage region
        ▼
  DisplayWidget (Qt widget)
        │  QPainter / QImage blit
        ▼
  Host screen
```

### Input Pipeline

```
Host keyboard / mouse / touchscreen
        │
        ▼
  Qt event (QKeyEvent, QMouseEvent, QTouchEvent)
        │
        ▼
  InputHandler
        │  keycode translation, touch point mapping
        ▼
  SpiceInput (input channel)
        │  SPICE key/mouse/touch messages
        ▼
  QEMU SPICE server → guest input driver
```

### VM Control Flow

```
Qt GUI action (e.g. "Pause" button)
        │
        ▼
  MainWindow slot
        │
        ▼
  QmpClient::sendCommand("stop")
        │  JSON over Unix socket: {"execute":"stop"}
        ▼
  QEMU process handles command
        │  JSON response: {"return":{}}
        ▼
  QmpClient emits signal
        │
        ▼
  MainWindow updates UI state
```

### VM Launch Sequence

```
User clicks "Start" (or CLI invocation)
        │
        ▼
  QemuConfig::buildArgs()
  - -machine android-x86,...
  - -accel hvf|kvm|whpx
  - -smp <cpus> -m <ram>
  - -drive file=system.img,...
  - -kernel bzImage -append "..."
  - -spice unix,path=/tmp/rex-spice-<id>.sock,...
  - -qmp unix:/tmp/rex-qmp-<id>.sock,server,wait=off
        │
        ▼
  QemuProcess::start()
  - QProcess::start(qemu_binary, args)
  - Wait for SPICE/QMP sockets to appear
        │
        ├──────────────────────────┐
        ▼                          ▼
  SpiceClient::connect()     QmpClient::connect()
  - spice_session_connect()  - QLocalSocket connect
  - Negotiate channels       - Read greeting banner
        │                          │
        ▼                          ▼
  Display + Input ready      VM control ready
        │
        ▼
  MainWindow shows live display
```

## Component Descriptions

### QemuProcess

Wraps `QProcess` to manage the QEMU subprocess. Responsibilities:

- Assembles the QEMU command line via `QemuConfig`
- Starts and monitors the child process
- Allocates unique socket paths for SPICE and QMP per VM instance
- Forwards stdout/stderr to an internal log buffer
- Emits signals on process exit or crash for GUI recovery handling

### QemuConfig

Builds the QEMU argument list from structured VM settings. Key areas:

- Machine type and acceleration backend (`-machine`, `-accel`)
- CPU topology and RAM (`-smp`, `-m`)
- Block devices: system image, data partition (`-drive`)
- Direct kernel boot: kernel image, initrd, cmdline (`-kernel`, `-initrd`, `-append`)
- SPICE server socket (`-spice`)
- QMP control socket (`-qmp`)
- Serial console (`-serial`)

### QmpClient

Implements the QMP (QEMU Machine Protocol) client over a Unix domain socket. Protocol details:

- QEMU sends a greeting JSON object on connect: `{"QMP": {"version": {...}, "capabilities": [...]}}`
- Client sends `{"execute": "qmp_capabilities"}` to leave negotiation mode
- Subsequent commands use `{"execute": "<command>", "arguments": {...}}`
- Responses arrive as `{"return": <value>}` or `{"error": {"class": "...", "desc": "..."}}`
- Asynchronous events (e.g. `STOP`, `RESUME`, `RESET`) are delivered out-of-band

Supported commands: `query-status`, `stop`, `cont`, `system_reset`, `savevm`, `loadvm`, `quit`.

### SpiceClient

Manages the spice-gtk session. Wraps `SpiceSession` and owns the display and input channels:

- `SpiceDisplay` — subscribes to the display channel, receives decoded frames, paints them onto `DisplayWidget`
- `SpiceInput` — translates Qt input events to SPICE input messages and injects them into the guest

### Qt GUI Components

| Component | File | Purpose |
|-----------|------|---------|
| MainWindow | `mainwindow.*` | Top-level window; sidebar with VM action buttons (start, pause, resume, stop, snapshot) |
| DisplayWidget | `display_widget.*` | SPICE display surface; scales framebuffer to widget size |
| InputHandler | `input_handler.*` | Translates Qt key/mouse/touch events for SPICE injection |
| KeymapEditor | `keymap_editor.*` | Visual editor for rebinding host keys to guest keycodes; game profiles |
| SettingsDialog | `settings_dialog.*` | Tabbed dialog: VM hardware, display, network, Frida |
| FridaPanel | `frida_panel.*` | Frida script editor with syntax highlighting and a live output console |

## Rust Middleware

The three middleware crates run independently from the Qt process:

| Crate | Role |
|-------|------|
| `rex-config` | Reads and writes TOML configuration files; validates schema; exposes typed structs |
| `rex-frida` | Downloads the Frida server binary for the guest architecture, pushes it via ADB, and keeps it running |
| `rex-update` | Polls a release feed, downloads a signed update bundle, and performs in-place binary replacement |

## Platform Acceleration

| Platform | Acceleration | Notes |
|----------|-------------|-------|
| macOS (Intel + Apple Silicon) | `-accel hvf` | Hypervisor.framework; no root required |
| Linux | `-accel kvm` | `/dev/kvm`; user must be in `kvm` group |
| Windows | `-accel whpx` | Windows Hypervisor Platform; enable in Windows Features |

QEMU falls back to `-accel tcg` if hardware virtualization is unavailable, but this is not supported for production use.

## Build System

```
CMakeLists.txt
├── FindQt6 (Core, Widgets, Network)
├── FindSPICE (spice-gtk, glib)
├── rexplayer  ← src/gui/ (all GUI + QEMU process + SPICE + QMP sources)
└── tests/     ← GTest executables

middleware/Cargo.toml  (separate workspace)
├── rex-config
├── rex-frida
└── rex-update
```

The Qt GUI and QEMU process manager are compiled into a single `rexplayer` binary. The Rust middleware crates are built separately and are not linked into the Qt binary.
