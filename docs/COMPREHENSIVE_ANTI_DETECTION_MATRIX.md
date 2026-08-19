# Comprehensive Anti-Detection Matrix & Substrate Safeguards

This document catalogs over **50 discrete anti-cheat and environment detection vectors** across hardware, kernel, runtime, graphics, and system layers, detailing how **RexPlayer v2** natively neutralizes each vector at the substrate level.

---

## 1. Complete Detection Vector Matrix

| # | Detection Vector | Standard Emulator Vulnerability | RexPlayer v2 Native Substrate Countermeasure |
| :- | :--- | :--- | :--- |
| **1** | **`glGetString(GL_RENDERER)`** | Returns `"llvmpipe"`, `"Mesa D3D12"`, `"VirtualBox"` | Intercepted in EGL/ANGLE HAL to return `"Adreno (TM) 750"` or `"Mali-G720"` |
| **2** | **`glGetString(GL_VENDOR)`** | Returns `"Mesa/X.org"` or `"VMware"` | Hardcoded to `"Qualcomm"` or `"ARM"` |
| **3** | **`glGetString(GL_EXTENSIONS)`** | Missing mobile-specific ASTC/ETC texture compression flags | Exposes full real-device GL extension table matching target OEM profile |
| **4** | **Vulkan `vkGetPhysicalDeviceProperties`** | Device name reveals host GPU (`"NVIDIA GeForce RTX 4090"`) | Spoofed via Vulkan Layer to report mobile GPU architecture (`"Adreno 750"`) |
| **5** | **CPU Topology (`/sys/devices/system/cpu`)** | Flat SMP cores (all cores same max frequency) | Emulates big.LITTLE / DynamIQ clusters (1 Prime + 3 Gold + 4 Silver cores with differing frequencies) |
| **6** | **CPU Info (`/proc/cpuinfo`)** | Reports `"Intel Core i7"` or `"AMD Ryzen"` | Custom kernel patches `/proc/cpuinfo` to report `"Qualcomm Snapdragon 8 Gen 3"` |
| **7** | **CPU Instruction Set (`MIDR_EL1`)** | Returns QEMU or x86 virtualization model ID | Intercepted in CPU model register traps to return genuine ARM Cortex-X4 IDs |
| **8** | **Storage Block Devices (`/sys/block/`)** | Contains `"vda"`, `"sda"`, `"loop0"`, `"ram0"` | Kernel patches sysfs to report legitimate UFS 4.0 flash storage (`"mmcblk0"`, `"sda"`) |
| **9** | **Mount Table (`/proc/self/mountinfo`)** | Reveals `overlay`, `tmpfs`, `lxc`, `waydroid` | `show_mountinfo()` strips all non-standard and container mounts |
| **10** | **Process Status (`/proc/self/status`)** | `TracerPid != 0` during debugging | `task_state()` permanently forces `TracerPid: 0` for monitored processes |
| **11** | **Debugger Self-Attachment (`ptrace`)** | `ptrace(PTRACE_TRACEME)` fails if debugged | Kernel intercepts `PTRACE_TRACEME` to return `0 (SUCCESS)` without locking |
| **12** | **Process Wait Channel (`/proc/self/wchan`)** | Shows `ptrace_stop` during breakpoint | Kernel replaces with `sys_epoll_wait` or `do_sigtimedwait` |
| **13** | **Network Listening Ports (`/proc/net/tcp`)** | Exposes port `27042` (Frida) or `5555` (ADB) | Kernel TCP seq_file filter hides privileged debugging ports |
| **14** | **Root Binaries on Disk** | Finds `/system/bin/su`, `/system/xbin/su` | **KernelSU:** Zero `su` files on disk; root granted purely in-kernel |
| **15** | **Direct Assembly Syscall (`svc #0`)** | Bypasses libc hooks to probe restricted files | Trapped directly inside custom Linux kernel (`fs/open.c`), returning `ENOENT` |
| **16** | **Package Manager API (`getInstalledPackages`)** | Lists MT Manager, Frida, Magisk, GameGuardian | AOSP PMS native whitelist filter returns only clean stock apps |
| **17** | **Direct App Directory Scanning** | Probes `/data/data/<package_name>` directly | Kernel `sys_openat` returns `ENOENT` for blacklisted package folders |
| **18** | **Memory Map Inspection (`/proc/self/maps`)** | Finds `frida-agent.so`, `linjector`, `gum-js` | Bionic/Kernel `/proc/self/maps` filter scrubs hooks and dynamic payloads |
| **19** | **Executable Anonymous Memory (`RWX`)** | Finds JIT trampolines from hook engines | Reflective loaders map stubs with legitimate `r-x` permissions matching `.text` |
| **20** | **System Properties (`ro.build.fingerprint`)** | Generic or emulator fingerprints | Hardcoded genuine Pixel 8 Pro release keys in `property_service.cpp` |
| **21** | **Bootloader Lock State (`ro.boot.flash.locked`)** | Reports `0` or `unlocked` | Hardcoded to `1 (locked)` |
| **22** | **Verified Boot State (`ro.boot.verifiedbootstate`)** | Reports `orange` or `yellow` | Hardcoded to `green (valid AVB 2.0)` |
| **23** | **Emulator Properties (`ro.kernel.qemu`)** | Reports `1` | Stripped completely; returns empty |
| **24** | **Virtual Device Nodes (`/dev/qemu_pipe`)** | Exists in standard emulators | Completely omitted in container runtime |
| **25** | **Sensor HAL Availability** | Zero sensors found | Synthetic Sensor HAL emulates 12 real hardware sensors |
| **26** | **Accelerometer Micro-Jitter** | Static exact `9.800000 m/s^2` | Injects realistic gravitational micro-noise ($9.806 \pm 0.005 \, \text{m/s}^2$) |
| **27** | **Gyroscope Thermal Drift** | Exactly `(0.0, 0.0, 0.0)` rad/s | Injects realistic micro-angular velocity drift |
| **28** | **Battery Charging State** | Permanently 100% AC plugged-in | Emulates realistic discharging curve (e.g. 87% discharging, $36.4^\circ\text{C}$) |
| **29** | **Battery Temperature (`/sys/class/power_supply`)**| Constant static temperature | Varies smoothly based on simulated CPU load ($32^\circ\text{C} - 41^\circ\text{C}$) |
| **30** | **Telephony Carrier Info** | No SIM or `"Android"` carrier | Telephony HAL returns valid operator codes (e.g. `SK Telecom 45005`) |
| **31** | **IMEI / IMSI / ICCID** | Missing or generic `000000000000000` | Generates mathematically valid Luhn-algorithm IMEI strings |
| **32** | **Wi-Fi BSSID / SSID** | `"AndroidWifi"` / `10.0.2.15` | Emulates real home Wi-Fi SSID and valid private IP ranges (`192.168.1.x`) |
| **33** | **Bluetooth Adapter State** | Missing or non-functional | Bluetooth HAL reports operational state with synthetic paired device list |
| **34** | **Camera Hardware (`android.hardware.camera2`)** | No camera or `"Virtual Camera"` | Full Camera2 HAL implementation supporting simulated video stream & auto-focus |
| **35** | **Touch Event Tool Type (`MotionEvent`)** | Reports `TOOL_TYPE_MOUSE` | `/dev/uinput` sets `ABS_MT_TOOL_TYPE = MT_TOOL_FINGER` |
| **36** | **Touch Pressure & Size** | Reports `pressure = 0`, `size = 0` | Emulates natural touch contact area (`pressure = 45..85`, `size = 12..18`) |
| **37** | **Micro-Touch Coordinates** | Perfect integer pixels (macro detection) | Injects sub-pixel human-finger jitter ($\pm 0.5$ px) |
| **38** | **Accessibility Service Scanning** | Detects active Auto-Clicker services | Native isolated services bypass `AccessibilityManager` entirely |
| **39** | **Display Refresh VSYNC Jitter** | Zero delta between frame timestamps | Injects microsecond display timer jitter matching real hardware panels |
| **40** | **Keymaster / KeyMint Attestation** | Missing TEE root certificates | Keymaster HAL provides valid OEM keystore and mocks Basic Integrity |
| **41** | **Play Integrity Verdict** | Fails `MEETS_DEVICE_INTEGRITY` | Injects certified fingerprint and valid DroidGuard token responses |
| **42** | **Widevine DRM Level** | Missing DRM or L3 only | Provides valid Widevine L3 keystore with correct media engine links |
| **43** | **Clock Monotonic vs Wall Time** | Speedhack timers modified | Enforces hardware RTC clock synchronization |
| **44** | **ARM Translation NEON Timing Delta** | Micro-benchmark timing reveals x86 JIT | Optimized ARM translation caches and host CPU AVX-512 acceleration |
| **45** | **Audio Buffer Timings** | Irregular buffer dropouts | Low-latency PipeWire / WASAPI shared ring-buffer (<15ms) |
| **46** | **Dual-Process Watchdog Heartbeat** | Process pause triggers watchdog kill | Hypervisor / Container freezes virtual clock during host debugger breaks |
| **47** | **Frida Server Default Port** | Port `27042` open | Randomized ephemeral ports or abstract Unix domain sockets |
| **48** | **Frida Thread Signatures** | `gmain`, `gum-js-loop` thread names | Masked to standard `UnityGfxDeviceWorker` or `RenderThread` |
| **49** | **Frida D-Bus Handshake** | `\x00auth\r\n` probe triggers response | Replaced with proprietary encrypted binary protocol |
| **50** | **Java Stack Trace Scanning** | Injected stack frames in Exception traces | Pure AOSP execution without ART method wrapper frames |

---

## 2. Verification & Automated CI Test Suite

To guarantee that no regression re-exposes any of these 50 detection vectors, the RexPlayer CI pipeline executes an automated **Stealth Verification Matrix (`tests/stealth/`)**:

```bash
# Automated Stealth Test Harness
cargo test -p rex-stealth-verifier -- \
  --test-sys-block \
  --test-direct-syscalls \
  --test-procfs-cleanliness \
  --test-sensor-jitter \
  --test-gl-strings
```

Every build is verified against a live suite of commercial detection APKs before release.
