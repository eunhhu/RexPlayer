# Windows x86-64 host assessment before runtime setup

This is an inventory-first handoff, not an instruction to provision or replace a
kernel. The integrated preview GUI is currently built for native Linux, not a
Windows `.exe`. Existing WSLg may provide a later test environment, but neither
WSLg presence nor an available Windows PC establishes Android runtime support.

## Stage 1: read-only inventory (no administrator changes)

Run in the user's normal PowerShell. Do not elevate to make a failed query pass.
Capture only these selected fields; omit usernames, machine names and unrelated
configuration from shared evidence.

```powershell
Get-CimInstance Win32_OperatingSystem | Select-Object Version, BuildNumber, OSArchitecture
Get-CimInstance Win32_Processor | Select-Object Name, AddressWidth, VirtualizationFirmwareEnabled, VMMonitorModeExtensions, SecondLevelAddressTranslationExtensions
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, AdapterCompatibility
Get-Command wsl.exe, adb.exe, scrcpy.exe, ffmpeg.exe, python.exe -ErrorAction SilentlyContinue | Select-Object Name
wsl.exe --version
wsl.exe --status
wsl.exe --list --verbose
```

A missing command or access-denied query is an unknown prerequisite, not approval
to install/elevate. These WSL commands inspect existing state only; do not invoke
`wsl -d`, install/update/shutdown/import/unregister, change default distributions,
or start a distro during this inventory. Avoid launching a Python Store alias;
use an already installed interpreter only.

If Python is already available, the checked-in read-only inspector produces the
bounded structured report without starting WSL or ADB:

```powershell
python -B runtime/capability_inspector.py --pretty
```

It reports existing `.wslconfig` kernel/kernelModules override presence without
printing their values. Do not upload the raw configuration or modify it.

## Stage 2: decide the test substrate from evidence

- Native Linux host: use the integrated-player runbook with a provisioned runtime
- Existing WSLg distro: obtain explicit approval before starting that exact distro;
  then inspect its kernel, Binder, uinput permissions and display/audio sockets
- No suitable isolated substrate: stop setup and select an isolation design

The historical custom `.wslconfig` kernel override affects **all** of the user's
WSL2 distributions. It is not an instance-local installation mechanism. This
preview must not silently overwrite it, shut down unrelated distributions, enable
Windows features, alter firewall rules or broadly expose devices. A dedicated
utility VM or independently supported WSL kernel path must be designed and tested
before claiming a safe Windows installer.

## Stage 3: explicit integration tests on an approved substrate

1. Build/run the exact reviewed commit, record tool/backend versions and selected
   device identity locally, and keep ADB off untrusted network interfaces
2. Render actual Android content through screenshot then H264 streaming; test
   startup cancellation, repeated starts, recording EOF, disconnection and close
3. Hear device-output audio; cancel/stop/restart it, test device changes, and measure
   audio/video timing rather than inferring playback from a process PID
4. Only after verifying the exact guest input-device route, opt into uinput and
   validate app-level contacts; exercise modifier/Escape/focus/resize/rotation and
   stale-frame cleanup. An ADB selection is not proof of a uinput route
5. Exercise actual native-package install, upgrade, failed healthcheck and rollback;
   preserve Android/user data and inspect for owned-process/device leaks
6. Test sleep/resume/reboot and quantify frame pacing, input/audio latency, idle
   resources and supported driver combinations

Administrative changes need a separate reviewed action/target and rollback plan.
None of these live-host results is implied by the cloud unit/process/codec tests.
