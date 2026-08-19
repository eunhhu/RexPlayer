# Hardware, Audio, Media & Multi-Instance Architecture

This document specifies the missing operational hardware pipelines in **RexPlayer v2** that determine real-world game compatibility, audiovisual synchronization, media decoding, network isolation, and multi-instance orchestration.

---

## 1. Ultra-Low Latency Audio Pipeline (Oboe / AAudio ──▶ PipeWire / WASAPI)

Audio latency is a notorious failure point in traditional Android emulators, introducing 150–300 ms delays that render rhythm games (e.g. Project Sekai, Arcaea) and fast-paced FPS titles unplayable.

### 1.1 Audio Pipeline Topology

```
┌─────────────────────────────────────────────────────────────┐
│ 1. Android Guest Audio Stack                                │
│    • Game Audio Engine (FMOD / Wwise / Unity Audio)         │
│    • AAudio / Oboe Native Audio Framework                   │
│    • AudioFlinger Native Server (HAL: audio.primary.default)│
└──────────────────────────────┬──────────────────────────────┘
                               │ (Direct Shared-Memory Circular Ring Buffer)
                               ▼
┌─────────────────────────────────────────────────────────────┐
│ 2. PulseAudio / PipeWire Bridge Socket                      │
│    • Linux Host: Direct Unix Socket to host PipeWire daemon  │
│    • Windows WSLg: RDP/PulseAudio Zero-Copy Virtual Channel │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│ 3. Host Sound Server Output                                 │
│    • Linux: PipeWire Pro Audio Profile (Buffer: 64 samples) │
│    • Windows: WASAPI Exclusive Mode Buffer (<10ms latency)  │
└─────────────────────────────────────────────────────────────┘
```

- **Target Latency:** Total round-trip audio latency strictly **< 15 ms**.
- **Sample Rate Locking:** Enforces exact 48,000 Hz / 16-bit stereo synchronization across Android AudioTrack and Host audio hardware, eliminating dynamic resampling artifacts and CPU spikes.

---

## 2. Hardware Video Decoding Pipeline (MediaCodec / In-Game Cutscenes)

Many modern mobile games (Genshin Impact, Honkai: Star Rail, Blue Archive, NIKKE) render story cutscenes via pre-rendered video files (H.264 / AVC, H.265 / HEVC, VP9, AV1). Traditional emulators often display a **black screen with audio only** because software decoders fail to link with guest surface textures.

```
[ In-Game Video Playback (MP4/H.264/HEVC) ]
                   │
                   ▼
  [ Android MediaCodec API (OMX / CCodec) ]
                   │
                   ▼ (Hardware Video Acceleration Bridge)
  [ VA-API / D3D12 Video Bridge Driver ]
                   │
                   ▼
  [ Host GPU Video Decode Engine (NVDEC / Intel QSV / AMD VCN) ]
                   │ (Direct DMA-BUF Texture Binding)
                   ▼
  [ SurfaceFlinger ──▶ Zero-Copy Display Buffer ]
```

### 2.1 Technical Strategy
1. **Mesa VA-API / D3D12 Video HAL:** RexPlayer integrates the `mesa-va-drivers` inside the container, mapping Android's `libstagefright_ccodec.so` directly to host hardware video decoders.
2. **Fallback Software Transcoder:** When unsupported exotic codecs are encountered, an in-memory high-throughput FFmpeg ARM64 fallback decoder guarantees zero-black-screen rendering.

---

## 3. 16KB Memory Page Size Compatibility (Android 15+ Future-Proofing)

Starting with Android 15, Google is transitioning the Android ecosystem from standard 4KB memory page sizes (`PAGE_SIZE=4096`) to **16KB page sizes (`PAGE_SIZE=16384`)** to improve memory bandwidth and application launch times on modern ARM silicon.

### 3.1 Emulation & Kernel Implications
- Native x86-64 Linux kernels and traditional virtualization run with 4KB pages.
- Native game `.so` binaries compiled with 16KB alignment will crash with `SIGBUS` or memory alignment errors if loaded into a non-compliant environment.

### 3.2 RexPlayer Compatibility Matrix
- **Custom Kernel Dual-Page Build:** The RexPlayer kernel build matrix supports compiling with `CONFIG_ARM64_16K_PAGES` for ARM64 targets and custom memory padding alignment for x86-64 NDK translation bridges.
- **Linker Alignment Fixer:** RexPlayer includes a native ELF header patcher (`rex-elf-align`) that inspects guest `.so` files during APK installation and ensures correct memory page alignment before execution.

---

## 4. Multi-Instance & Sandboxed Virtual Environments

For game automation, multi-account analysis, and parallel security testing, RexPlayer supports running multiple independent Android instances with zero resource leakage.

```
┌───────────────────────────────────────────────────────────────────────────┐
│                        REXPLAYER INSTANCE ORCHESTRATOR                    │
└───────────────────────────────────────────────────────────────────────────┘
               │                                            │
               ▼                                            ▼
┌──────────────────────────────┐             ┌──────────────────────────────┐
│ Instance #0 (Main Analysis)  │             │ Instance #1 (Multi-Account)  │
├──────────────────────────────┤             ├──────────────────────────────┤
│ • Container ID: rex_0        │             │ • Container ID: rex_1        │
│ • Rootfs: Overlay UpperDir 0 │             │ • Rootfs: Overlay UpperDir 1 │
│ • Android ID: a1b2c3d4e5f6001│             │ • Android ID: f6e5d4c3b2a1002│
│ • MAC: 02:42:C0:A8:F0:01     │             │ • MAC: 02:42:C0:A8:F0:02     │
│ • Proxy: 127.0.0.1:8080(Burp)│             │ • Proxy: SOCKS5 Direct Res   │
│ • Wayland Display: wayland-0 │             │ • Wayland Display: wayland-1 │
└──────────────────────────────┘             └──────────────────────────────┘
               │                                            │
               └──────────────────────┬─────────────────────┘
                                      ▼
             [ Shared Read-Only Base System Image (AOSP Base) ]
```

### 4.1 OverlayFS Copy-on-Write Storage
- All instances share a single, immutable base system image (`system.img` / base rootfs).
- Each instance maintains an independent `upperdir` storing only modified user data (`/data/`).
- **Disk Savings:** 10 cloned instances consume only ~2 GB of additional disk space rather than 40 GB.

---

## 5. Network Routing, SSL Pinning & Proxy Architecture

For security researchers analyzing game network packets, RexPlayer provides built-in network redirection without requiring root certificate warnings on the guest OS:

### 5.1 Transparent Proxying (Per-Instance Routing)
- Each instance can be bound to an independent network interface or SOCKS5 / HTTP proxy (e.g. Burp Suite, Charles, mitmproxy).
- **Kernel-Level Redirection:** Network traffic is intercepted using `iptables` / `nftables` inside the container network namespace, routing all TCP/UDP traffic to the specified proxy port without modifying Android system Wi-Fi settings.

### 5.2 Universal SSL Pinning Bypass (Bionic & Conscrypt Native Hook)
- Instead of relying on vulnerable Frida scripts that trigger anti-cheat hooks, RexPlayer injects a custom root CA certificate directly into Android's native Conscrypt keystore at boot time and modifies `bionic/libc/`'s OpenSSL/BoringSSL verification callbacks to enforce automatic trust for the researcher's proxy certificate.
