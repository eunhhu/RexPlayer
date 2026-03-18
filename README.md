# RexPlayer

**A native Android player that runs QEMU as a subprocess with SPICE display and QMP control.**

RexPlayer launches a QEMU process per VM, connects to it via SPICE for display/input, and controls it via the QMP JSON protocol. A Qt 6 GUI wraps the full lifecycle. Rust middleware crates handle configuration, Frida server management, and self-update.

## Architecture

```
┌─────────────────────────────────────────┐
│  Qt 6 GUI                               │
│  MainWindow, DisplayWidget, KeymapEditor│
│  SettingsDialog, FridaPanel             │
├─────────────────────────────────────────┤
│  SPICE Client          QMP Client       │
│  spice-gtk display     JSON/socket VM   │
│  + input forwarding    control          │
├─────────────────────────────────────────┤
│  QEMU Process Manager                   │
│  QemuProcess, QemuConfig                │
│  subprocess lifecycle + CLI args        │
├─────────────────────────────────────────┤
│  QEMU (subprocess)                      │
│  HVF (macOS) | KVM (Linux) | WHPX (Win)│
└─────────────────────────────────────────┘

Rust middleware (out-of-process)
  rex-config  — TOML configuration
  rex-frida   — Frida server lifecycle
  rex-update  — Self-update mechanism
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full design document.

## Features

- **Hardware virtualization** — HVF on macOS, KVM on Linux, WHPX on Windows; no TCG fallback
- **SPICE display** — hardware-accelerated remote display via spice-gtk
- **QMP control** — full VM lifecycle (start, pause, resume, stop, snapshot) over JSON socket
- **Qt 6 GUI** — right-sidebar action buttons, modern settings dialog
- **Game keymap editor** — rebind host keys to guest keycodes with a visual editor
- **Frida script editor** — write and inject Frida scripts directly from the GUI
- **Direct kernel boot** — pass `--kernel` + `--system-image` for fast startup without firmware
- **Rust middleware** — config management, Frida server lifecycle, and OTA self-update

## Building

### Prerequisites

| Platform | Requirements |
|----------|-------------|
| All | CMake 3.25+, Qt 6 (Core, Widgets, Network), QEMU, Rust toolchain |
| Linux | GCC 13+ or Clang 17+, KVM-enabled kernel, spice-gtk, glib |
| macOS | Xcode 15+, Hypervisor.framework entitlement, spice-gtk via Homebrew |
| Windows | MSVC 2022+, Windows Hypervisor Platform enabled, spice-gtk |

#### Installing spice-gtk

```bash
# macOS (Homebrew)
brew install spice-gtk glib

# Ubuntu/Debian
sudo apt install libspice-client-gtk-3.0-dev libglib2.0-dev

# Fedora/RHEL
sudo dnf install spice-gtk3-devel glib2-devel
```

### Build Steps

```bash
# Clone
git clone https://github.com/rexplayer/rexplayer.git
cd rexplayer

# Build the Qt GUI + QEMU process manager
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Build / test the Rust middleware workspace
cd middleware
cargo build --release
cargo test --workspace
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `REX_ENABLE_TESTS` | `ON` | Build C++ test suite |
| `REX_ENABLE_GUI` | `ON` | Build Qt GUI (requires Qt 6) |

## Usage

```bash
# Launch with a system image and kernel
./build/rexplayer --system-image system.img --kernel bzImage

# Specify a custom QEMU binary
./build/rexplayer --qemu-binary /usr/local/bin/qemu-system-x86_64 \
                  --system-image system.img --kernel bzImage

# Override CPU / RAM / display
./build/rexplayer --system-image system.img --kernel bzImage \
                  --cpus 4 --ram 4096 --width 1080 --height 1920

# Show all options
./build/rexplayer --help
```

### Key CLI Options

| Option | Description |
|--------|-------------|
| `--qemu-binary <path>` | Path to the QEMU executable (auto-detected if omitted) |
| `--system-image <path>` | Android system image to boot |
| `--kernel <path>` | Linux kernel image (bzImage) |
| `--cpus <n>` | Number of virtual CPUs (default: 2) |
| `--ram <mb>` | RAM in megabytes (default: 2048) |
| `--width <px>` | Display width in pixels (default: 1080) |
| `--height <px>` | Display height in pixels (default: 1920) |
| `--config <path>` | TOML configuration file |

### Configuration (TOML)

```toml
[vm]
qemu_binary = "/usr/local/bin/qemu-system-x86_64"
vcpus = 4
ram_mb = 4096
system_image = "system.img"
kernel = "bzImage"
kernel_cmdline = "androidboot.hardware=rex console=ttyS0"

[display]
width = 1080
height = 1920
dpi = 320

[frida]
enabled = true
auto_update = true
port = 27042
```

## Project Structure

```
rexplayer/
├── src/
│   ├── gui/          # Qt 6 GUI
│   │   ├── mainwindow.*        # Main window + sidebar action buttons
│   │   ├── display_widget.*    # SPICE display surface
│   │   ├── input_handler.*     # Keyboard + touch input forwarding
│   │   ├── keymap_editor.*     # Game keymap editor
│   │   ├── settings_dialog.*   # VM + display + network settings
│   │   ├── frida_panel.*       # Frida script editor + console
│   │   ├── qemu_process.*      # QEMU subprocess lifecycle
│   │   ├── qemu_config.*       # CLI argument builder
│   │   ├── qmp_client.*        # QMP JSON socket client
│   │   ├── spice_client.*      # SPICE session management
│   │   ├── spice_display.*     # SPICE display channel
│   │   └── spice_input.*       # SPICE input channel
├── middleware/       # Rust workspace
│   ├── rex-config/   # TOML configuration management
│   ├── rex-frida/    # Frida server lifecycle manager
│   └── rex-update/   # Self-update mechanism
├── android/          # AOSP guest device tree + kernel configs
├── tests/            # C++ tests
├── packaging/        # Linux (.desktop), macOS (Info.plist), Windows (NSIS)
├── scripts/          # Build, fetch, package scripts
└── cmake/            # Find modules (Qt6, SPICE, QEMU)
```

## Rust Middleware

| Crate | Description |
|-------|-------------|
| rex-config | TOML-based configuration read/write with schema validation |
| rex-frida | Downloads, installs, and manages the Frida server binary inside the guest |
| rex-update | Checks for RexPlayer updates and performs in-place binary replacement |

```bash
# Run all middleware tests
cd middleware && cargo test --workspace

# Individual crates
cargo test -p rex-config
cargo test -p rex-frida
cargo test -p rex-update
```

## Frida Integration

The Frida panel in the GUI lets you write and inject scripts without leaving the app:

```bash
# Connect from the host CLI (via ADB-forwarded port)
frida-ps -H localhost:27042

# Attach to a process
frida -H localhost:27042 -n com.example.app -l script.js
```

The `rex-frida` crate manages the server lifecycle: it downloads the correct Frida server binary for the guest architecture, pushes it into the VM, and keeps it running.

## License

MIT License. See [LICENSE](LICENSE) for details.

## Acknowledgments

- [QEMU](https://www.qemu.org) — the hypervisor substrate
- [SPICE](https://www.spice-space.org) — remote display and input protocol
- [spice-gtk](https://gitlab.freedesktop.org/spice/spice-gtk) — GTK SPICE client library
- [Frida](https://frida.re) — dynamic instrumentation toolkit
