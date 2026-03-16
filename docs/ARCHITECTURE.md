# RexPlayer Architecture

This document describes the internal architecture of RexPlayer, a lightweight Android app player that uses native hypervisor APIs instead of QEMU.

## Design Philosophy

1. **No QEMU** — Use OS-native hypervisor APIs directly (KVM, HVF, WHPX)
2. **Minimal device set** — ~12 purpose-built virtio devices, no legacy ISA/PCI baggage
3. **Split-language architecture** — C++ for VMM core (low-level, latency-sensitive), Rust for device backends (safety, concurrency)
4. **Lock-free hot paths** — No global locks on vCPU execution or virtqueue processing
5. **Platform-native I/O** — io_uring (Linux), dispatch_io (macOS), IOCP (Windows)

## Layer Diagram

```
┌──────────────────────────────────────────────────────────────┐
│                      Layer 5: Qt 6 GUI                       │
│                                                              │
│  MainWindow          DisplayWidget        SettingsDialog     │
│  - Menu/Toolbar      - Framebuffer        - VM config        │
│  - VM controls         rendering          - Display          │
│  - Status bar        - Touch/keyboard     - Network          │
│                        mapping            - Frida             │
│                      InputHandler                            │
│                      - Qt key → Linux     - Multi-touch      │
│                        keycode mapping    - WASD gaming       │
├──────────────── cxx FFI (rex-ffi) ───────────────────────────┤
│                   Layer 4: Rust Middleware                    │
│                                                              │
│  rex-devices (virtio backends)    rex-network                │
│  ├── virtio-blk                   ├── DHCP server            │
│  ├── virtio-net                   ├── DNS relay              │
│  ├── virtio-gpu                   └── UserNet NAT            │
│  ├── virtio-input                                            │
│  ├── virtio-vsock                 rex-frida                  │
│  ├── virtio-snd                   └── Server lifecycle       │
│  ├── virtio-balloon                                          │
│  ├── virtio-console               rex-config (TOML)          │
│  ├── virtio-mmio (transport)       rex-filesync              │
│  └── virtqueue (helpers)           rex-update                │
├──────────────────────────────────────────────────────────────┤
│                    Layer 3: C++ VMM Core                     │
│                                                              │
│  Vm                  MemoryManager        DeviceManager      │
│  - vCPU thread       - mmap/VirtualAlloc  - I/O port         │
│    management        - GPA ↔ HVA           dispatch          │
│  - State machine     - Huge pages         - MMIO dispatch    │
│  - Start/pause/stop                                          │
│                      BootLoader            SnapshotManager   │
│                      - x86 bzImage         - RLE compress    │
│                      - ARM64 Image         - Atomic save     │
│                      - Direct kernel boot  - Register serial │
│                                                              │
│  CpuOptimizer        IoOptimizer          InstanceManager    │
│  - Topology detect   - Batch virtqueue    - Multi-VM         │
│  - vCPU pinning      - Interrupt coalesce - CoW clone        │
│  - TSC scaling       - Adaptive sizing    - State persist    │
│                                                              │
│  Legacy Devices                                              │
│  ├── UART 16550 (serial console)                             │
│  ├── i8042 (PS/2 keyboard stub)                              │
│  ├── RTC (MC146818 real-time clock)                          │
│  └── PCI Host Bridge (config space access)                   │
├──────────────────────────────────────────────────────────────┤
│                 Layer 2: HAL (Hypervisor Abstraction)         │
│                                                              │
│  IHypervisor interface                                       │
│  ├── KvmHypervisor      (Linux /dev/kvm ioctl)              │
│  ├── HvfHypervisor       (macOS Hypervisor.framework x86)    │
│  ├── HvfArm64Hypervisor  (macOS Hypervisor.framework ARM64)  │
│  └── WhpxHypervisor     (Windows WHvCreatePartition)         │
│                                                              │
│  IVcpu interface         IMemoryManager interface            │
│  - run()                 - map_region()                      │
│  - get/set_regs()        - unmap_region()                    │
│  - get/set_sregs()       - gpa_to_hva()                     │
│  - inject_interrupt()                                        │
│  - get/set_msr()                                             │
├──────────────────────────────────────────────────────────────┤
│                   Layer 1: GPU Pipeline                      │
│                                                              │
│  IRenderer interface                                         │
│  ├── SoftwareRenderer   (CPU fallback, 2D resource mgmt)    │
│  ├── VirglRenderer      (virglrenderer → host OpenGL)        │
│  └── VenusRenderer      (Mesa Venus → host Vulkan/MoltenVK) │
│                                                              │
│  GpuBridge              ShaderCache       Display            │
│  - Rust GPU → C++       - FNV-1a hash     - Double buffer   │
│  - Resource lifecycle   - Disk cache      - Present callback │
│  - Scanout management   - Load/store      - Resize           │
├──────────────────────────────────────────────────────────────┤
│                  Platform Abstraction                         │
│                                                              │
│  Async I/O                    Threading                      │
│  ├── io_uring (Linux)         - Thread pinning               │
│  ├── dispatch_io (macOS)      - Thread naming                │
│  └── IOCP (Windows)           - Priority management          │
└──────────────────────────────────────────────────────────────┘
```

## Data Flow

### vCPU Execution Loop

```
vCPU Thread                    VMM Core                   Device Backend
     │                            │                            │
     │── HAL::run() ──────────────│                            │
     │                            │                            │
     │◄─ VcpuExit(IoAccess) ──────│                            │
     │                            │── DeviceManager            │
     │                            │   ::dispatch_io() ─────────│
     │                            │                            │── handle I/O
     │                            │◄─────── response ──────────│
     │── HAL::run() ──────────────│                            │
     │                            │                            │
     │◄─ VcpuExit(MmioAccess) ───│                            │
     │                            │── FFI::handle_mmio() ──────│
     │                            │   (cxx bridge to Rust)     │── virtio-mmio
     │                            │◄─────── DeviceResponse ────│   dispatch
     │── HAL::run() ──────────────│                            │
```

### Guest Display Pipeline

```
Guest GPU Driver                 Host
     │                            │
     │── virtio-gpu cmd ──────────│
     │   RESOURCE_CREATE_2D       │── GpuBridge
     │   TRANSFER_TO_HOST_2D      │   ::resource_create_2d()
     │   SET_SCANOUT              │   ::transfer_to_host_2d()
     │   RESOURCE_FLUSH           │   ::set_scanout()
     │                            │   ::resource_flush()
     │                            │        │
     │                            │        ▼
     │                            │   SoftwareRenderer
     │                            │   or VirglRenderer
     │                            │        │
     │                            │        ▼
     │                            │   Display (double-buffer)
     │                            │        │
     │                            │        ▼
     │                            │   DisplayWidget (QPainter)
     │                            │        │
     │                            │        ▼
     │                            │   Host Screen
```

### Network Path

```
Guest App
  │
  ▼
virtio-net (Rust)
  │
  ├── TAP backend (bridged networking, requires root)
  │     └── Host kernel TAP device
  │
  └── UserNet backend (userspace NAT, no root needed)
        ├── DHCP server (10.0.2.0/24 subnet)
        ├── DNS relay (forwards to host resolver)
        └── NAT table (connection tracking, port forwarding)
              └── Host network stack
```

## C++ / Rust Boundary

The FFI boundary is defined in `middleware/rex-ffi/src/lib.rs` using the [cxx](https://cxx.rs) crate:

**C++ → Rust** (vCPU exit handling):
- `handle_mmio(MmioRequest) → DeviceResponse`
- `handle_io(IoRequest) → DeviceResponse`
- `middleware_init()`, `middleware_tick()`

**Rust → C++** (callbacks):
- `inject_irq(IrqRequest)` — interrupt injection
- `read_guest_memory() / write_guest_memory()` — DMA
- `gpu_resource_create_2d()`, `gpu_set_scanout()`, `gpu_resource_flush()` — GPU operations

All FFI types are plain structs with no heap allocation across the boundary.

## Virtio Transport

All virtio devices use the **virtio-mmio** transport (not virtio-pci). Each device is mapped to a dedicated MMIO address range in guest physical memory. The transport implements the full virtio state machine:

```
RESET → ACKNOWLEDGE → DRIVER → FEATURES_OK → DRIVER_OK
```

Virtqueues use split-ring format with the `VIRTIO_F_EVENT_IDX` feature for interrupt suppression.

## Snapshot Format

```
Offset  Content
0x00    SnapshotHeader (magic="REXS", version, timestamp, layout)
0x30    VcpuSnapshot[0] (X86Regs + X86Sregs)
...     VcpuSnapshot[N-1]
var     RLE-compressed guest RAM
var     Device state (reserved for future)
```

RLE encoding optimizes for zero-filled pages (common in VM memory):
- **Run**: `[count:u32][byte:u8]` — N copies of the same byte
- **Literal**: `[0:u32][count:u32][data...]` — raw byte sequence

## Thread Model

| Thread | Purpose | Pinning |
|--------|---------|---------|
| Main | Qt event loop, GUI rendering | — |
| vCPU-0 | Bootstrap processor execution | P-core preferred |
| vCPU-N | Application processor execution | P-core preferred |
| I/O | Async disk/network I/O | E-core or any |

The CpuOptimizer detects hybrid architectures (Intel Alder Lake+, Apple M-series) and preferentially pins vCPU threads to performance cores.

## Memory Layout (x86_64)

```
GPA               Content
0x0000_0000       Real-mode IVT / BDA
0x0000_7000       Linux boot_params
0x0002_0000       Kernel command line
0x0010_0000       Protected-mode kernel (bzImage)
0x0400_0000       Initrd / initramfs
...               Guest RAM (up to configured size)
```

## Memory Layout (ARM64)

```
GPA               Content
0x4000_0000       RAM base
0x4008_0000       Kernel Image
0x4400_0000       Device Tree Blob (DTB)
0x4800_0000       Initrd / initramfs
```

## Build System

```
CMakeLists.txt
├── Corrosion (FetchContent) ─── middleware/Cargo.toml
├── GoogleTest (FetchContent)
├── rex_hal        ← src/hal/ (platform-conditional sources)
├── rex_vmm        ← src/vmm/ (VM core + optimizers)
├── rex_devices_legacy ← src/devices/
├── rex_gpu        ← src/gpu/ (renderers + bridge)
├── rex_platform   ← src/platform/ (per-OS source)
├── rexplayer      ← src/gui/ (Qt 6, optional)
└── tests/         ← GTest executables
```

Rust crates are compiled by Corrosion and linked into the C++ binary. The cxx bridge generates both C++ headers and Rust bindings at build time.
