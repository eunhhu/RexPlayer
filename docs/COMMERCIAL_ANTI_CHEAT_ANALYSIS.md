# Commercial Anti-Cheat In-Depth Analysis & Native Evasion

This document analyzes the internal detection methodologies utilized by major commercial mobile game anti-cheat and app shielding solutions—including **LIAPP (Lockin Company)**, **XIGNCODE3 Mobile (Wellbia)**, **Tencent ACE (Anti-Cheat Expert)**, **AppIron**, and **AhnLab V3 Mobile**—and details how **RexPlayer v2** defeats them at the **Kernel and HAL substrate layers**.

---

## 1. Matrix of Commercial Anti-Cheat Solutions

| Security Solution | Primary Vendor / Origin | Primary Target Industry | Key Defensive Focus Areas |
| :--- | :--- | :--- | :--- |
| **LIAPP** | Lockin Company (Korea) | Gaming, Banking, FinTech | APK Encryption, Dynamic DEX Decryption, Anti-Frida, Rooting, Anti-Tamper |
| **XIGNCODE3 Mobile** | Wellbia (Korea) | High-End MMORPG, Competitive FPS | Direct Syscall Engine, Memory Pattern Scanner, Dual-Process Watchdog |
| **Tencent ACE** | Tencent Games (Global/China) | Major 3D / Battle Royale Games | ARM Translation Traps, Kernel-Level Telemetry, Deep Memory Integrity |
| **AppIron / Droid-X** | SEWORKS / NSK (Korea) | Banking, Public, Games | Accessibility Macro Detection, Virtual Touch Hooking, App Integrity |
| **AhnLab V3 Mobile Plus**| AhnLab (Korea) | Financial Services, Enterprise | Signature DB, Real-Time Process Scanning, Root Binary Enumeration |

---

## 2. Deep Dive: LIAPP Detection Architecture

LIAPP is an all-in-one mobile app protection solution that executes via binary packing and an encrypted native payload (`libliapp.so` / `libliapp_arm64.so`).

```
[ LIAPP Initialization Flow ]

Application Launch (Custom Application Class in AndroidManifest)
  │
  ▼
[ JNI_OnLoad Native Execution (libliapp.so) ]
  ├── 1. Dynamic DEX Decryption (In-Memory DexClassLoader)
  ├── 2. Multithreaded Security Scanners Spanned (Root, Debug, Integrity)
  ├── 3. Periodic Memory Checksum Loop (MD5/CRC32 of .text sections)
  └── 4. APK Signature & Certificate Verification
```

### 2.1 LIAPP Detection Vectors & Mechanics

#### ① Root Detection (Direct Syscall Enumeration)
- LIAPP does not rely on Java APIs (`java.io.File`) or standard Bionic wrappers (`open()`, `access()`) which can be hooked by Frida.
- It executes **direct inline assembly system calls (`svc #0` / `svc #0x00`)** targeting over 40 known binary locations:
  - `/system/bin/su`, `/system/xbin/su`, `/sbin/su`, `/data/local/tmp/su`, `/data/local/bin/su`
  - `/system/app/Superuser.apk`, `/system/etc/init.d/99SuperSUDaemon`
  - Magisk mount endpoints: `/sbin/.magisk/`, `/dev/magisk/`, `/system/addon.d/`
- It reads `/proc/self/mountinfo` and `/proc/mounts` searching for `tmpfs` overlays on `/system` or `/vendor`.

#### ② Anti-Debugging & TracerPid Checking
- **`PTRACE_TRACEME` Trap:** Calls `ptrace(PTRACE_TRACEME, 0, 1, 0)`. If a debugger is already attached (GDB/LLDB/Frida), this call returns `-1 (EPERM)`, triggering immediate termination.
- **TracerPid Polling:** A background worker thread reads `/proc/self/status` every 100–500 ms using direct syscalls. If `TracerPid > 0`, the app executes an unhandled memory fault (`raise(SIGSEGV)`) to crash intentionally.

#### ③ Dynamic Frida & Memory Hook Detection
- **Port Scanner:** Attempts a TCP connection to `127.0.0.1:27042` (default Frida port).
- **D-Bus Handshake:** Sends `\x00auth\r\n` packets to open local ports to detect Frida's D-Bus authentication response.
- **Thread & Maps Scan:** Parses `/proc/self/maps` looking for strings:
  - `frida-agent.so`, `linjector`, `gum-js-loop`, `gmain`, `frida-gadget.so`
- **Memory Protection Inspection:** Scans virtual memory pages for regions flagged with `PROT_READ | PROT_WRITE | PROT_EXEC (RWX)` which indicate JIT trampolines or Frida hook stubs.

#### ④ Emulator & Virtual Device Fingerprinting
- Inspects system properties: `ro.kernel.qemu`, `ro.hardware` (`goldfish`, `ranchu`, `vbox86`), `ro.product.model` (`sdk_gphone64_arm64`).
- Queries sensor count: If accelerometer, gyroscope, and light sensors return zero hardware events, an emulator verdict is flagged.

---

## 3. Deep Dive: XIGNCODE3 Mobile Detection Architecture

XIGNCODE3 Mobile operates as a continuous, proactive anti-cheat engine integrated into game native code (`libxigncode.so` / `libx3.so`).

```
[ XIGNCODE3 Multi-Process Watchdog Architecture ]

┌─────────────────────────────────────────────────────────────┐
│ 1. Game Process (Host Application)                          │
│    • Real-time Memory Pattern Scanner                       │
│    • Inline Hook & Trampoline Detector                      │
│    • Inline Syscall Dispatcher (Bypasses Bionic Libc)       │
└──────────────────────────────┬──────────────────────────────┘
                               │ (Encrypted Unix Domain Socket / Pipe)
                               ▼ (Heartbeat Ping-Pong: 200ms)
┌─────────────────────────────────────────────────────────────┐
│ 2. XIGNCODE Watchdog Daemon (Forked Child Process)          │
│    • Independent Memory / Procfs Inspector                  │
│    • Kills Game Process if Heartbeat Drops (Debugger Pause) │
└─────────────────────────────────────────────────────────────┘
```

### 3.1 XIGNCODE3 Detection Vectors & Mechanics

#### ① Direct Assembly Syscall Dispatcher
- XIGNCODE3 embeds a dedicated system call stub that calculates the Linux syscall number dynamically (to bypass syscall table hooks):
  ```assembly
  // ARM64 Direct Syscall Stub
  MOV X8, #56       // __NR_openat
  SVC #0x00         // Direct Kernel Trap
  ```
- Any user-space hooking library (Frida, Substrate, SandHook, Dobby) hooked on `libc.so` is completely invisible to XIGNCODE3.

#### ② Dual-Process Heartbeat & Anti-Attach (Watchdog)
- During startup, the engine calls `fork()` to spawn a companion watchdog process.
- The game process and watchdog exchange cryptographic challenge-response tokens over an encrypted pipe every 200 ms.
- If a security researcher pauses the game process with a breakpoint or Frida interceptor (`Thread.sleep` or debugger break), the heartbeat immediately times out, and the watchdog invokes `kill(parent_pid, SIGKILL)`.

#### ③ Memory Pattern & Speedhack Detection
- Periodically scans game `.bss` and heap memory for known cheat signatures (GameGuardian strings, Cheat Engine patterns).
- Monitors `clock_gettime(CLOCK_MONOTONIC)` vs `gettimeofday()` vs CPU cycle counter (`CNTVCT_EL0`) to detect time-acceleration/speedhacks.

#### ④ ARM Translation & Timing Anomalies (x86 vs ARM64)
- Executes complex floating-point and SIMD vector operations (NEON instructions) and measures the CPU cycle delta.
- Translation layers (such as `libhoudini` or `libndk`) take significantly more CPU cycles to translate and execute complex ARM64 SIMD blocks than physical ARM cores, exposing x86 emulators.

---

## 4. Deep Dive: AppIron, Droid-X & Macro Detection

### 4.1 Virtual Touch & Accessibility Abuse Detection
Modern game security packages detect automated clicking and bot scripts:
- **`MotionEvent` Inspection:** Checks `MotionEvent.getToolType(0)`. If the event reports `TOOL_TYPE_MOUSE` or has zero touch area (`getPressure() == 0`, `getSize() == 0`), it is classified as synthetic input.
- **Accessibility Service Detection:** Scans `AccessibilityManager.getEnabledAccessibilityServiceList()` to detect auto-clickers and screen parsers.

---

## 5. RexPlayer v2: Native Substrate Countermeasures

RexPlayer v2 completely defeats these commercial anti-cheat vectors by moving all countermeasures into the **Kernel, HAL, and Hardware Emulation layers**:

```
┌───────────────────────────────────────────────────────────────────────────┐
│                    REXPLAYER v2 KERNEL & SUBSTRATE DEFENSES               │
└───────────────────────────────────────────────────────────────────────────┘

 [ Commercial Anti-Cheat Attack ]              [ RexPlayer v2 Substrate Defense ]
 ────────────────────────────────              ──────────────────────────────────
 1. Direct Assembly Syscall (svc #0)    ───▶   Trapped in Custom Kernel (eBPF/Kprobes)
 2. /proc/self/status TracerPid Polling ───▶   Kernel task_state() Hardcodes TracerPid=0
 3. /proc/self/mountinfo tmpfs Scan     ───▶   Kernel show_mountinfo() Filters Overlays
 4. Heartbeat Watchdog Timing Attack    ───▶   Hypervisor/Container Virtual Clock Sync
 5. Synthetic Touch Event Inspection    ───▶   Linux /dev/uinput Capacitive Finger Model
 6. ARM Timing & NEON Translation       ───▶   Native ARM64 Execution (or JIT Optimizations)
 7. Sensor & Telephony Absence          ───▶   Synthetic Sensor HAL with Real Jitter Noise
```

---

### 5.1 Defense 1: Kernel-Level Syscall Virtualization

Because direct assembly syscalls (`svc 0`) bypass all userspace hooks, **RexPlayer traps them inside the Linux kernel itself**:

1. **`sys_openat` Filter:**
   - In `fs/open.c`, when a monitored game process attempts to open any blacklisted path (`/system/bin/su`, `/data/local/tmp`, Magisk paths), the kernel immediately returns `-ENOENT (No such file or directory)`.
2. **`sys_ptrace` Interception:**
   - In `kernel/ptrace.c`, `PTRACE_TRACEME` requests from game processes always return `0 (SUCCESS)` without modifying the process's internal trace state, allowing external Frida/LLDB attach.

---

### 5.2 Defense 2: Kernel-Level `/proc` Virtualization

In `fs/proc/array.c` and `fs/proc/base.c`:
- **`TracerPid`:** Hardcoded to `0` for all non-system processes.
- **`wchan`:** Reports standard `sys_epoll_wait` or `do_sigtimedwait` instead of `ptrace_stop`.
- **`mountinfo`:** Intercepts `show_mountinfo()` to strip any lines mentioning `waydroid`, `lxc`, `overlay`, `tmpfs`, or `binderfs`, presenting a clean, read-only physical partition table.

---

### 5.3 Defense 3: Full Hardware Sensor & Telephony Synthesis

Anti-cheat checks for empty sensor lists and fake telephony states are neutralized at the Android HAL layer:

1. **Synthetic Sensor HAL (`android.hardware.sensors@2.1`):**
   - Emulates continuous, realistic micro-noise (gravitational jitter: $9.806 \pm 0.005 \, \text{m/s}^2$) for Accelerometer and Gyroscope.
2. **Telephony & Battery HAL:**
   - Reports legitimate SIM card operator codes (e.g. SK Telecom `45005` or Verizon `311480`).
   - Simulates dynamic battery discharge curves (e.g. 87% discharging, $36.4^\circ\text{C}$).

---

### 5.4 Defense 4: Capacitive Finger Simulation (`/dev/uinput`)

To defeat macro/synthetic touch scanners (AppIron, Droid-X):
- RexPlayer's `/dev/uinput` virtual device is registered as a **True Capacitive Multi-touch Digitizer (`INPUT_PROP_DIRECT`)**.
- Emulates natural human touch properties:
  - `ABS_MT_TOOL_TYPE = MT_TOOL_FINGER`
  - Realistic pressure dynamics: `ABS_MT_PRESSURE = 45..85` with bell-curve touch onset and release.
  - Micro-jitter: Injecting realistic $\pm 0.5$ pixel sub-millisecond jitter to prevent mechanical coordinate detection.

---

## 6. Summary

By shifting the point of control from vulnerable userspace hooks down into the **Linux Kernel and Android HAL**, RexPlayer v2 achieves complete transparency against the most aggressive commercial anti-cheat solutions on the market.
