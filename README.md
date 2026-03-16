# RexPlayer

**A lightweight, high-performance Android app player built on native hypervisor APIs.**

RexPlayer runs Android applications on your desktop by leveraging OS-native hypervisors (KVM on Linux, Hypervisor.framework on macOS, WHPX on Windows) instead of QEMU. This eliminates the overhead of legacy device emulation, the Big QEMU Lock, and JIT-based CPU translation — delivering near-native performance with minimal resource usage.

## Key Features

- **Native Hypervisor Acceleration** — Direct KVM/HVF/WHPX integration, no QEMU dependency
- **Minimal VMM** — ~12 purpose-built virtio devices instead of hundreds of legacy devices
- **GPU Passthrough** — virglrenderer (OpenGL) and Venus (Vulkan) with shader caching
- **Built-in Frida** — Integrated Frida Server for dynamic instrumentation and security research
- **Cross-Platform** — Linux, macOS (Intel + Apple Silicon), Windows
- **Multi-Instance** — Run multiple Android VMs simultaneously with copy-on-write cloning
- **Fast Boot** — Direct kernel boot (no BIOS/UEFI), snapshot save/restore with RLE compression
- **Qt GUI** — Touch input emulation, keyboard mapping, screen rotation, APK installation

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

## Performance vs QEMU-based Players

| Area | Improvement | How |
|------|-------------|-----|
| CPU | No TCG fallback | Always hardware virtualization |
| Memory | 30-50% less RAM | ~12 devices vs hundreds |
| I/O | 20-40% faster | Lock-free virtqueues, io_uring/IOCP |
| Boot | 2-4s faster | Direct kernel boot, no firmware |
| GPU | Near-native | virglrenderer/Venus passthrough |

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

# Build C++ (CMake auto-detects platform and hypervisor)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Build Rust middleware
cd middleware
cargo build --release

# Run tests
cargo test --workspace          # 208 Rust tests
ctest --test-dir ../build       # C++ tests
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `REX_ENABLE_TESTS` | `ON` | Build test suite |
| `REX_ENABLE_GUI` | `ON` | Build Qt GUI (requires Qt 6) |
| `REX_ENABLE_GPU` | `OFF` | Link virglrenderer/Venus for GPU acceleration |

### GPU Acceleration (Optional)

```bash
# Install virglrenderer (Linux)
sudo apt install libvirglrenderer-dev

# Build with GPU support
cmake -B build -DCMAKE_BUILD_TYPE=Release -DREX_ENABLE_GPU=ON
```

## Usage

```bash
# Launch with a kernel and system image
./build/rexplayer --kernel bzImage --system-image system.img

# Custom configuration
./build/rexplayer --config config.toml --cpus 4 --ram 4096

# CLI options
./build/rexplayer --help
```

### Configuration (TOML)

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
│   ├── gpu/          # GPU renderers (Software, Virgl, Venus) + bridge
│   ├── platform/     # OS abstraction (threading, async I/O)
│   └── gui/          # Qt 6 GUI (display, input, settings)
├── middleware/       # Rust workspace
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

## Frida Integration

RexPlayer includes built-in Frida Server support for security research:

```bash
# Frida connects via vsock (no network overhead)
frida-ps -H localhost:27042

# Attach to a process
frida -H localhost:27042 -n com.example.app -l script.js
```

The Frida manager handles automatic version detection, download from GitHub releases, and vsock bridge setup.

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

## Contributing

See [CONTRIBUTING.md](docs/CONTRIBUTING.md) for development guidelines.

## License

MIT License. See [LICENSE](LICENSE) for details.

## Acknowledgments

- [rust-vmm](https://github.com/rust-vmm) — Virtio crate foundation
- [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) — OpenGL GPU passthrough
- [Mesa Venus](https://docs.mesa3d.org/drivers/venus.html) — Vulkan GPU passthrough
- [cxx](https://cxx.rs) — Safe C++/Rust interop
- [Frida](https://frida.re) — Dynamic instrumentation toolkit
