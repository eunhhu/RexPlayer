# Native Stealth & Zygisk Evasion Strategy

This document details the architectural strategy for **natively embedding the capabilities of Magisk, Zygisk, Shamiko, Hide My Applist (HMA), and Play Integrity Fix directly into RexPlayer v2's Kernel, Bionic C Library, ART (Android Runtime), and AOSP Framework layers**.

By baking these evasion techniques natively into the operating system rather than injecting them as third-party userspace modules, RexPlayer v2 eliminates all module footprints, race conditions, and injection signatures.

---

## 1. Deconstruction of Existing Modding & Evasion Stacks

Understanding how the current state-of-the-art rooting and evasion tools operate reveals why they remain vulnerable to sophisticated anti-cheat scanners:

```
[ Traditional Evasion Stack: Layered Patchwork ]

┌─────────────────────────────────────────────────────────────┐
│  Target Game Process                                        │
│    ▲ (Scans for injected libraries & hooked memory maps)    │
├────┴────────────────────────────────────────────────────────┤
│  LSPosed Framework (Java ART Hooking) ──▶ [HMA Module]       │
│    ▲ (Relies on Zygote memory injection: signature vector)  │
├────┴────────────────────────────────────────────────────────┤
│  Zygisk Framework (Native C++ Injection) ──▶ [Shamiko]      │
│    ▲ (Hooks forkAndSpecialize: /proc/self/maps footprint)   │
├────┴────────────────────────────────────────────────────────┤
│  Magisk Daemon (magiskd) + OverlayFS Magic Mount            │
│    ▲ (Uses tmpfs mounts & sepolicy live-patching)           │
├────┴────────────────────────────────────────────────────────┤
│  Stock Linux Kernel (Unaware of stealth requirements)       │
└─────────────────────────────────────────────────────────────┘
```

### 1.1 Magisk Rooting & Daemon Mechanics
- **Init Hijack (`magiskinit`):** Intercepts PID 1 `/init`, mounts an in-memory root filesystem (`tmpfs`), and patches `sepolicy` on the fly to grant `magiskd` unrestricted domain permissions.
- **Magic Mount:** Uses `bind` mounts and `overlayfs` to overlay `/system` without modifying the physical partition.
- **Vulnerability:** Anti-cheat scanners inspect `/proc/self/mountinfo`, `/proc/mounts`, and `stat()` timing anomalies to detect overlay filesystems and atypical mount namespace clones.

### 1.2 Zygisk & Shamiko Mechanics
- **Zygote Companion Injection:** Zygisk hooks `Zygote`'s `forkAndSpecializeCommon()` in ART to load a native shared object into every newly spawned app process.
- **Shamiko Unmount Technique:**
  1. For apps on the "DenyList", Shamiko calls `unshare(CLONE_NEWNS)` to isolate the process mount namespace.
  2. Unmounts all Magisk-related `tmpfs` and `/sbin` paths.
  3. Erases its own `.so` name from memory structures.
- **Vulnerability:** Advanced anti-cheats check memory protection flags (`PROT_READ | PROT_EXEC`) on anonymous memory pages and detect unshare timing discrepancies during process initialization.

### 1.3 Hide My Applist (HMA / LSPosed)
- **Framework Hooking:** Hooks `ApplicationPackageManager.getInstalledPackages()`, `queryIntentActivities()`, and `getPackageInfo()` via Xposed/LSPosed to hide packages like `com.topjohnwu.magisk`, `org.lsposed.manager`, `bin.mt.plus` (MT Manager), and Frida.
- **Vulnerability:** Games bypass Java framework APIs altogether by querying the Linux kernel directly via raw system calls (`openat(AT_FDCWD, "/data/data/com.topjohnwu.magisk", O_RDONLY)` or `/proc/<pid>/cmdline`).

---

## 2. RexPlayer v2: The Native OS-Level Architecture

Instead of running an ongoing "cat-and-mouse" injection battle inside userspace, RexPlayer v2 modifies the lower-level OS substrates (Kernel, Bionic Libc, AOSP, and ART) to return clean, authentic real-device data **by default**.

```
[ RexPlayer v2: Native Substrate Architecture ]

┌──────────────────────────────────────────────────────────────────┐
│  Target Game Process                                             │
│  (Executes raw syscalls, libc checks, Java APIs, and mem scans)  │
└────────────────────────────────┬─────────────────────────────────┘
                                 │
  ┌──────────────────────────────┼──────────────────────────────┐
  ▼                              ▼                              ▼
[ 1. AOSP Framework ]       [ 2. Bionic Libc ]         [ 3. Linux Kernel ]
• Native Package Filter     • Syscall Interceptors     • KernelSU Syscall Root
• SystemProperties Spoof    • Virtual File Masking     • TracerPid=0 / Anti-Ptrace
• ART Native Dumper Hook    • Sanitized /proc/maps     • BinderFS & Ashmem Native
└────────────────────────────────┬──────────────────────────────┘
                                 │
┌────────────────────────────────▼─────────────────────────────────┐
│  4. Rex-Core Host Engine (Zero Guest Footprint)                  │
│  • Memory Inspection via Host eBPF / Kernel Probes               │
│  • Out-of-band Frida Controller via Abstract Unix Sockets        │
└──────────────────────────────────────────────────────────────────┘
```

---

## 3. Subsystem Implementation Strategies

### 3.1 Kernel Layer: Fileless Root & Syscall Cloaking

#### ① KernelSU Integration (Zero Filesystem Root)
- Superuser privileges are granted purely through a modified kernel system call (e.g. customized `prctl` or custom syscall number).
- When RexPlayer's internal tools require root, they invoke the kernel hook with a cryptographic token. The kernel modifies the calling process's `struct cred` in kernel memory (`commit_creds()`).
- **Result:** No `su` binary exists anywhere in `/system/bin`, `/system/xbin`, or `/data/local/tmp`. Scanners searching for root files find nothing.

#### ② Kernel-Level `/proc` Virtualization
In `fs/proc/` within the custom WSL2 / Linux kernel:
1. **`/proc/<pid>/status`:** Forces `TracerPid` to `0` whenever queried by non-whitelisted processes, neutralizing anti-debugging checks.
2. **`/proc/<pid>/mountinfo`:** Automatically filters out any container, overlay, or virtual filesystem mount lines. To the game, the mount table appears identical to an encrypted, read-only ext4/EROFS Android partition.
3. **`/proc/net/tcp` & `/proc/net/tcp6`:** Filters out listening ports associated with debugging (e.g. port `27042`, `5555`).

```c
/* Kernel Hook: Clean mountinfo representation */
static int show_mountinfo(struct seq_file *m, struct vfsmount *mnt)
{
    struct mount *r = real_mount(mnt);
    /* Strip overlay, tmpfs, and emulator-specific mountpoints */
    if (is_stealth_process(current) && is_virtual_mount(r)) {
        return 0; /* Omit from output */
    }
    return original_show_mountinfo(m, mnt);
}
```

---

### 3.2 Bionic C Library Layer: Low-Level Syscall Virtualization

Commercial game protectors (such as Tencent ACE and NetEase) invoke raw syscalls (via inline assembly `svc 0`) or call Bionic libc directly to bypass Java framework hooks. 

In RexPlayer's custom Bionic (`bionic/libc/`):

#### ① File Path Blacklist Masking (`openat`, `facit`, `stat`)
When any process queries paths associated with root or analysis tools, Bionic intercepts the call before execution:
```c
// bionic/libc/bionic/open.cpp
int __openat(int fd, const char* pathname, int flags, int mode) {
    if (is_blacklisted_security_path(pathname)) {
        errno = ENOENT; // Return 'No such file or directory'
        return -1;
    }
    return __real_openat(fd, pathname, flags, mode);
}
```
**Masked Paths Include:**
- `/system/app/Superuser.apk`, `/system/xbin/su`, `/system/bin/failsafe/su`
- `/data/local/tmp/frida*`, `/data/local/tmp/gdb*`
- `/system/lib/libmonochrome.so`, `/system/etc/hosts` modifications

#### ② Memory Map Sanitization (`/proc/self/maps`)
When a game reads its own `/proc/self/maps` to detect memory hooks or injected `.so` files (e.g. Frida gadget or inline hooks):
- Bionic's `read()` wrapper filters out memory segments mapped to Frida agents or hooking stubs, presenting a contiguous, legitimate memory map.

---

### 3.3 AOSP Framework & ART Layer: Native Cloaking & Hooking

#### ① Native App Visibility Firewall (Built-in HMA)
In `frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java`:
- Instead of using LSPosed hooks to hide apps, the package manager itself maintains a native isolation policy.
- Unless an app is an explicit system component or explicitly granted permission in RexPlayer settings, `getInstalledPackages()`, `queryIntentActivities()`, and `resolveContentProvider()` return a curated list containing only stock system apps and Google Play services.

#### ② Embedded ART Instrumentation (Zero-Frida Extraction)
In `art/runtime/native/` and `art/runtime/interpreter/`:
- **Native DEX & Il2Cpp Dumper:** An internal hook is placed inside `art::DexFile::Open()` and `art::ArtMethod::Invoke()`.
- When an APK with packed/encrypted code executes, ART dumps the decrypted DEX / ELF memory buffer directly to a private host shared-memory region before execution begins.
- **Benefit:** Reverse engineers extract decrypted game code without needing to attach Frida or trigger runtime memory integrity trips.

#### ③ Immutable System Properties (`resetprop` Baked-In)
In `system/core/init/property_service.cpp`:
- All security-sensitive properties are hardcoded and locked to genuine OEM values:
```ini
ro.boot.flash.locked = 1
ro.boot.verifiedbootstate = green
ro.boot.veritymode = enforcing
ro.build.type = user
ro.build.tags = release-keys
ro.debuggable = 0
ro.secure = 1
```
- Calls to `__system_property_get()` for emulator properties (`ro.kernel.qemu`, `ro.boot.hardware=goldfish`) return empty/default values.

---

### 3.4 Keymaster & Play Integrity Mocking

Google's **Play Integrity API** and hardware attestation evaluate device trustworthiness via hardware Keymaster/KeyMint HAL:

1. **Hardware Keystore Emulation:**
   - RexPlayer provides a customized `android.hardware.keymaster` HAL implementation.
   - For basic integrity and device integrity verdicts, it supplies valid root certificates from certified OEM profiles (e.g. Pixel 8 Pro release keys).
2. **Bootloader State Mocking:**
   - Kernel command-line parameters (`/proc/cmdline`) are scrubbed of any `androidboot.mode=emulator` flags, ensuring the verification engine reports a locked bootloader with valid AVB 2.0 signatures.

---

## 4. Feature Comparison: Add-on Modules vs. RexPlayer Native Engine

| Evasion Target | Add-on Mod Method (Zygisk + Shamiko + HMA) | **RexPlayer v2 Native Substrate** |
| :--- | :--- | :--- |
| **Root Binary Detection** | Moves `su` to virtual overlay; unmounts for denied apps | **KernelSU: No `su` binary ever exists on disk** |
| **TracerPid Anti-Debug** | Requires Zygisk hook or native ptrace interceptor | **Kernel `/proc` patch: TracerPid permanently 0** |
| **App List Scanning** | LSPosed Java method hooks (bypassed via raw syscalls) | **Bionic libc + AOSP PMS native filtering** |
| **Memory Map Scanning** | In-memory signature scrubbing (race condition prone) | **Kernel/Bionic maps virtualization** |
| **Module Footprint** | Zygisk `.so` injection visible in ART stack traces | **Zero guest modules; execution inside pure AOSP ART** |
| **Maintenance Overhead** | Breaks whenever Magisk, Zygisk, or Android updates | **Self-contained, reproducible custom OS image** |

---

## 5. Summary

By replacing fragile, userspace injection modules with **native Kernel and AOSP substrate modifications**, RexPlayer v2 achieves an unprecedented level of stealth:
1. **Zero Injection Signatures:** No hook libraries injected into target game processes.
2. **Syscall-Proof Cloaking:** Games executing raw assembly system calls are handled by the modified kernel itself.
3. **Turnkey Experience:** Security researchers and gamers get a fully rootable, fully instrumented environment that requires zero manual module configuration.
