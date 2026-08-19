# Stealth & Security Analysis Architecture

RexPlayer v2 is designed as a premier environment for **mobile game security research, CTF challenges, reverse engineering, and dynamic instrumentation**. This document details the defense-in-depth architecture implemented to bypass modern commercial anti-cheat solutions and runtime integrity checks.

---

## 1. Threat Model & Anti-Cheat Capabilities

Modern game security systems (e.g. Tencent ACE, NetEase, Nexon BlackCipher, BattlEye Mobile, EasyAntiCheat, AppIron, Droid-X) inspect multiple operating system layers:

```
┌─────────────────────────────────────────────────────────────┐
│ 1. Filesystem & Artifact Scanning                           │
│    • Checks for /system/bin/su, /data/local/tmp/frida       │
│    • Checks for emulator paths (nox, bluestacks, goldfish)  │
├─────────────────────────────────────────────────────────────┤
│ 2. Process & Kernel Inspection                              │
│    • Reads /proc/self/status for TracerPid != 0             │
│    • Calls ptrace(PTRACE_TRACEME) to block debuggers        │
│    • Inspects /proc/self/wchan, /proc/net/tcp for ports     │
├─────────────────────────────────────────────────────────────┤
│ 3. Memory & Virtualization Signature Checks                 │
│    • Scans /proc/self/maps for GumJS / Frida gadget strings │
│    • Checks CPU instruction timing and hypervisor flags     │
│    • Checks system build properties and DRM security level  │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Kernel-Level Root Evasion: KernelSU Integration

### 2.1 Why Traditional Magisk / SuperSU Fails
- Traditional root frameworks create files in `/system/bin` or bind-mount virtual filesystems that leave distinct filesystem traces or `mount` namespace anomalies.
- Security scanners search for known su binaries, zygote module paths, and `/dev` block modifications.

### 2.2 The KernelSU Solution
RexPlayer integrates **KernelSU** directly into the custom WSL2 / Linux kernel source:

```
[ Target Game Process ]
        │
        ▼ (Attempts to scan filesystem for root)
  [ /system/bin ] ──▶ 100% Clean (No 'su' binary exists on disk)
  [ /system/xbin ] ──▶ 100% Clean
  [ /proc/mounts ] ──▶ 100% Clean
        │
        ▼ (Authorized RexPlayer Security Daemon)
  [ Issues Special Kernel System Call ]
        │
        ▼ (Kernel Mode Trap)
  [ KernelSU Kernel Hook ]
        │ • Validates process UID against internal whitelist
        │ • Elevates credentials directly in kernel memory (`commit_creds()`)
        ▼
  [ Instant Root Execution without Filesystem Artifacts ]
```

- **Invisible to Scanners:** No userspace files, no modified init scripts, no anomalous mount points.

---

## 3. Kernel-Level Anti-Debugging Defeat

### 3.1 `TracerPid` & `ptrace` Spoofing
When a reverse engineer attaches Frida, GDB, or LLDB to a game process, the Linux kernel sets `TracerPid` in `/proc/<pid>/status` to the debugger's PID. Games periodically check this file and terminate if `TracerPid > 0`.

#### RexPlayer Kernel Hook:
In the RexPlayer custom kernel (`fs/proc/array.c`), the procfs status generator is patched:
```c
/* RexPlayer Stealth Kernel Patch */
static inline void task_state(struct seq_file *m, struct pid_namespace *ns,
                              struct pid *pid, struct task_struct *p, bool user)
{
    ...
    /* Force TracerPid to 0 for protected game processes */
    if (is_rex_stealth_target(p)) {
        seq_printf(m, "TracerPid:\t0\n");
    } else {
        seq_printf(m, "TracerPid:\t%d\n", tpid);
    }
    ...
}
```
- The game always reads `TracerPid: 0`, even when actively attached and instrumented by Frida.
- `ptrace(PTRACE_TRACEME)` calls by the game are intercepted to return `0 (SUCCESS)` without locking the process against external debuggers.

---

## 4. Covert Frida Instrumentation (`rex-frida-stealth`)

RexPlayer includes a specialized Frida bridge that eliminates standard signatures:

| Detection Vector | Standard Frida | **RexPlayer Covert Frida** |
| :--- | :--- | :--- |
| **Default Port** | `27042` TCP | **Randomized Ephemeral Ports or Abstract Unix Domain Sockets** |
| **Server Binary Name** | `frida-server` | **Renamed to innocuous system thread names (e.g. `kworker/u:2`)** |
| **Thread Names** | `gmain`, `gum-js-loop` | **Masked to standard ART / Unity worker thread names** |
| **Memory Maps (`/proc/self/maps`)** | Contains `frida-agent.so` | **Cloaked memory pages using custom in-memory reflective loaders** |
| **D-Bus Handshake** | Plaintext `LIBFRIDA` auth | **Custom encrypted IPC protocol** |

---

## 5. Device Fingerprint & DRM Spoofing

RexPlayer builds upon a curated set of verified OEM properties:

```ini
# /system/build.prop
ro.product.brand=google
ro.product.name=husky
ro.product.device=husky
ro.product.model=Pixel 8 Pro
ro.product.manufacturer=Google
ro.build.fingerprint=google/husky/husky:14/UQ1A.240205.004/11269751:user/release-keys
ro.boot.flash.locked=1
ro.boot.verifiedbootstate=green
ro.boot.veritymode=enforcing
ro.hardware=husky
ro.kernel.qemu=0
```

- **Emulator Flag Scrubbing:** Removes all references to `goldfish`, `qemu`, `ranchu`, `vbox`, or `vport`.
- **DRM Mocking:** Provides a valid Widevine L3 / Mock L1 keystore to pass media integrity tests.
