# RexPlayer

**A native-hypervisor Android player prototype built around a C++ VMM core and a separate Rust device workspace.**

RexPlayer is currently best understood as a prototype: the C++ runtime can create a VM, boot x86 kernels directly, render through the software display path, and drive a Qt shell. The Rust middleware crates are actively developed and tested, but they are not linked into the default native binary yet.

## Current Capabilities

- **Native hypervisor backends in tree** — KVM (Linux), WHPX (Windows), HVF x86 (Intel macOS)
- **Direct x86 kernel boot** — bzImage loading plus snapshot save/restore scaffolding
- **Qt GUI shell** — framebuffer display, screenshots, settings, and VM lifecycle controls
- **Software renderer** — a working 2D fallback path with unit coverage
- **Rust middleware workspace** — virtio, network, Frida, update, config, and filesync crates with standalone tests
- **Instance management utilities** — cloning, persistence, and metadata tracking

## Not Yet Wired Into The Default Native Runtime

- **System-image boot and virtio device registration**
- **Rust middleware / cxx FFI integration**
- **Virgl / Venus passthrough in the shipped binary**
- **Frida console, APK install, and guest file sync from the GUI**
- **ARM64 guest boot and Apple Silicon host support**

## Architecture

```
┌─────────────────────────────────────┐
│  Qt 6 GUI                          │
│  Display, Input, Settings          │
├─── cxx FFI ─────────────────────────┤
│  Rust Middleware (7 crates)         │
│  virtio devices, network, frida    │
├─────────────────────────────────────┤
│  C++ VMM Core                       │
│  VM lifecycle, memory, boot, snap  │
├─────────────────────────────────────┤
│  HAL (Hypervisor Abstraction)       │
│  KVM │ HVF (x86+ARM64) │ WHPX     │
├─────────────────────────────────────┤
│  GPU: Software │ Virgl │ Venus     │
└─────────────────────────────────────┘
```

See [ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full design document.

## Design Goals

| Area | Intended Direction | How |
|------|-------------|-----|
| CPU | Avoid TCG fallback | Always hardware virtualization |
| Memory | Keep the device model small | Purpose-built devices instead of a full QEMU machine |
| I/O | Prefer host-native async I/O | io_uring / dispatch_io / IOCP abstractions |
| Boot | Faster startup | Direct kernel boot, no firmware |
| GPU | Optional acceleration | Experimental virglrenderer / Venus renderers |

## Building

### Prerequisites

| Platform | Requirements |
|----------|-------------|
| All | CMake 3.25+, Rust toolchain, Qt 6 |
| Linux | GCC 13+ or Clang 17+, KVM-enabled kernel |
| macOS | Xcode 15+, Hypervisor.framework entitlement |
| Windows | MSVC 2022+, Windows Hypervisor Platform enabled |

### Build Steps

```bash
# Clone
git clone https://github.com/rexplayer/rexplayer.git
cd rexplayer

# Build the native prototype runtime
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Build / test the Rust workspace separately
cd middleware
cargo build --release

# Run tests
cargo test --workspace          # 208 Rust tests
ctest --test-dir ../build       # C++ tests
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `REX_ENABLE_TESTS` | `ON` | Build C++ test suite |
| `REX_ENABLE_GUI` | `ON` | Build Qt GUI (requires Qt 6) |
| `REX_ENABLE_GPU` | `OFF` | Build experimental Virgl/Venus sources |
| `REX_ENABLE_EXPERIMENTAL_MIDDLEWARE` | `OFF` | Import Rust crates into the native CMake build graph |

### GPU Acceleration (Experimental)

```bash
# Install virglrenderer (Linux)
sudo apt install libvirglrenderer-dev

# Build with experimental GPU sources enabled
cmake -B build -DCMAKE_BUILD_TYPE=Release -DREX_ENABLE_GPU=ON
```

The default runtime still falls back to the software renderer today.

## Usage

```bash
# Launch with a kernel image
./build/rexplayer --kernel bzImage

# Override CPU / RAM / display from the CLI
./build/rexplayer --kernel bzImage --cpus 4 --ram 4096 --width 1080 --height 1920

# CLI options
./build/rexplayer --help
```

`--config` and `--system-image` are rejected by the current native runtime because those code paths are not wired up yet.

### Configuration (Planned TOML shape)

```toml
[vm]
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
│   ├── hal/          # Hypervisor Abstraction Layer (KVM, HVF, WHPX)
│   ├── vmm/          # VMM Core (VM, memory, boot, snapshot, optimizers)
│   ├── devices/      # Legacy devices (UART, i8042, RTC, PCI)
│   ├── gpu/          # Software renderer + experimental Virgl/Venus sources
│   ├── platform/     # OS abstraction (threading, async I/O)
│   └── gui/          # Qt 6 GUI (display, input, settings)
├── middleware/       # Rust workspace (tested separately; not linked by default)
│   ├── rex-devices/  # Virtio device backends (blk, net, gpu, input, ...)
│   ├── rex-ffi/      # C++ ↔ Rust FFI bridge (cxx)
│   ├── rex-config/   # TOML configuration management
│   ├── rex-frida/    # Frida Server lifecycle manager
│   ├── rex-network/  # NAT, DHCP, DNS relay
│   ├── rex-filesync/ # Host ↔ guest file synchronization
│   └── rex-update/   # Self-update mechanism
├── android/          # AOSP guest device tree + kernel configs
├── tests/            # C++ tests (HAL, GPU) + Rust tests (208 total)
├── packaging/        # Linux (.desktop), macOS (Info.plist), Windows (NSIS)
├── scripts/          # Build, fetch, package scripts
└── cmake/            # Find modules (KVM, HVF, WHPX, Virgl, Venus)
```

## Virtio Device Stack

| Device | Description | Tests |
|--------|-------------|-------|
| virtio-blk | Block storage (raw image backend) | 7 |
| virtio-net | Networking (TAP/userspace NAT) | 21 |
| virtio-gpu | 2D/3D graphics, scanout, capset | 30 |
| virtio-input | Touchscreen + keyboard (multi-touch) | 26 |
| virtio-vsock | Host-guest sockets (ADB, Frida) | 14 |
| virtio-snd | PCM audio playback/capture | 21 |
| virtio-balloon | Dynamic memory management | 16 |
| virtio-console | Serial console I/O | — |
| virtio-mmio | MMIO transport layer | 10 |

## Frida Workspace Crate

The repository includes a Frida manager crate aimed at security-research workflows:

```bash
# Frida connects via vsock (no network overhead)
frida-ps -H localhost:27042

# Attach to a process
frida -H localhost:27042 -n com.example.app -l script.js
```

That crate is not connected to the default GUI/runtime path yet.

## Testing

```bash
# All Rust tests (208 tests across 7 crates)
cd middleware && cargo test --workspace

# C++ unit tests
ctest --test-dir build --output-on-failure

# Specific crate
cargo test -p rex-devices    # 150 tests
cargo test -p rex-network    # 28 tests
cargo test -p rex-frida      # 10 tests
cargo test -p rex-config     # 5 tests
cargo test -p rex-filesync   # 7 tests
cargo test -p rex-update     # 8 tests
```

## License

MIT License. See [LICENSE](LICENSE) for details.

## Acknowledgments

- [rust-vmm](https://github.com/rust-vmm) — Virtio crate foundation
- [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) — OpenGL GPU passthrough
- [Mesa Venus](https://docs.mesa3d.org/drivers/venus.html) — Vulkan GPU passthrough
- [cxx](https://cxx.rs) — Safe C++/Rust interop
- [Frida](https://frida.re) — Dynamic instrumentation toolkit
