# RexPlayer Packaging and Release Strategy

**Status:** proposed production strategy, informed by the 2026-08-20 runtime proof. It is not an implemented installer or a release-readiness claim.

## 1. Decision summary

RexPlayer should ship as a small, signed host application plus separately versioned runtime artifacts. The host application, WSL/Linux substrate, custom kernel, Android image, and user data must not be collapsed into one mutable bundle.

The first distributable channel should be an **internal validation package**, not a consumer release. The current proof establishes Android boot, ADB, and raw `/dev/uinput` delivery to Android InputReader on two validation substrates. It does not establish graphics, audio, application-visible touch, latency, lifecycle recovery, multi-instance isolation, broad hardware compatibility, or safe one-click installation.

### Release principles

1. **Evidence before claims.** Every release capability links to an executable gate and sanitized evidence.
2. **Immutable artifacts.** Kernel, root filesystem, Android OCI image, and host binaries are addressed by version and digest.
3. **Least privilege.** The UI is unprivileged. Administrative operations are narrow, explicit, and separated from normal runtime control.
4. **Atomic update and rollback.** A new runtime is health-checked before it becomes current; last-known-good artifacts remain available.
5. **Transparent security posture.** The default package is a detectable Android lab substrate. It does not ship concealment, fingerprint spoofing, integrity bypasses, or hidden instrumentation.
6. **No destructive global defaults.** Existing WSL distributions, Docker installations, network policy, and user data are not modified without a scoped migration and rollback record.

## 2. Artifact model

Each release is described by a signed `release-manifest.json` containing semantic version, channel, supported host builds, artifact URLs, SHA-256 digests, OCI digests, minimum kernel capabilities, schema version, and rollback compatibility.

| Artifact | Format | Versioning and trust boundary |
| --- | --- | --- |
| Host UI and controller | Windows x86-64 MSI; Linux DEB/RPM | SemVer, Authenticode or repository signature |
| Privileged provisioning helper | Separate signed executable/package | Same release train, narrow command surface |
| WSL root filesystem | Deterministic tar archive | Content digest and migration schema |
| WSL kernel | `bzImage`, config, source commit, SBOM | Exact Microsoft WSL source commit and SHA-256 |
| Android runtime | OCI image | Registry digest, architecture, Android/API version |
| Input/runtime helpers | Native binaries inside managed runtime | Built from the same tagged source and manifest |
| Policy and defaults | Signed JSON/TOML | Schema-versioned; user overrides stored separately |
| Evidence bundle | Text logs plus checksum manifest | Sanitized, immutable, not required at runtime |

The currently pinned third-party ReDroid image is acceptable only for laboratory reproduction. A public package requires a license review, redistribution record, vulnerability scan, and either an approved upstream image or a reproducible first-party AOSP image. Tags are never trusted without a digest.

## 3. Windows packaging

### 3.1 Recommended package

Use a **signed WiX Toolset v4 MSI with a small bootstrapper**, not an MSIX-only or NSIS-only design.

- MSI provides repair, upgrade codes, component ownership, uninstall records, and enterprise deployment semantics.
- The bootstrapper performs prerequisite detection before elevation: supported Windows build, WSL availability, virtualization support, free disk, incompatible `.wslconfig`, and port/network policy.
- Only the provisioning phase elevates. The daily host application runs as the user.
- A portable ZIP may be published for diagnostics, but it must not provision kernels or privileged services.

### 3.2 WSL custom-kernel constraint

The proven `.wslconfig` `kernel=` setting is **global to the current user's WSL2 distributions**, not scoped to the RexPlayer distribution. That is acceptable for an explicitly opted-in laboratory package but is a blocker for an unattended consumer installer.

The internal alpha flow may:

1. Inspect and preserve the complete existing `.wslconfig`.
2. Display the affected distributions and require explicit consent.
3. Install a versioned kernel under a RexPlayer-owned directory.
4. Write an exact backup path and transaction record before changing the setting.
5. run `wsl.exe --shutdown` only after consent and only after work-state warnings.
6. Boot a dedicated RexPlayer distribution and run kernel, BinderFS, uinput, and Android health checks.
7. Restore the previous config automatically if health checks fail.

A production Windows release must choose and validate one of these paths:

- a supported WSL mechanism that does not replace the kernel for unrelated distributions;
- an upstream/default WSL kernel that already exposes the required Android primitives;
- or a dedicated RexPlayer utility VM with an explicit resource and security model.

Until that decision passes compatibility testing, Windows packaging remains preview-only.

### 3.3 Windows data layout

```text
%ProgramFiles%\RexPlayer\                 signed, read-only host binaries
%ProgramData%\RexPlayer\artifacts\       versioned manifests and shared artifacts
%LOCALAPPDATA%\RexPlayer\config\         per-user settings
%LOCALAPPDATA%\RexPlayer\logs\           redacted operational logs
%LOCALAPPDATA%\RexPlayer\wsl\            dedicated distribution storage/VHDX
%LOCALAPPDATA%\RexPlayer\updates\         staged update slots
```

Credentials are stored through Windows Credential Manager, never in config files or command-line arguments. User Android data is separate from replaceable runtime layers.

### 3.4 Windows uninstall and rollback

Uninstall must distinguish application removal, runtime removal, and user-data deletion. The default preserves user data. It must:

- stop only RexPlayer-owned processes and its dedicated distribution;
- restore `.wslconfig` only when the transaction record proves RexPlayer changed it and the current value still matches the RexPlayer value;
- never delete or unregister unrelated WSL distributions;
- remove Windows Firewall rules and scheduled tasks by exact identifier;
- retain a readable removal report;
- require a second explicit confirmation before deleting Android user data.

## 4. Linux packaging

Start with signed **DEB and RPM repositories** for supported distributions. AppImage may package an unprivileged UI later, but it is not suitable as the authority for kernel devices, systemd units, udev policy, or container lifecycle. Arch PKGBUILD and community formats should follow after the core package contract stabilizes.

Suggested layout:

```text
/usr/bin/rexplayer                    unprivileged launcher
/usr/lib/rexplayer/                   versioned host/runtime helpers
/usr/lib/systemd/system/rexplayerd.service
/usr/lib/udev/rules.d/                narrowly scoped device policy
/etc/rexplayer/                       administrator policy
/var/lib/rexplayer/                   OCI/runtime state
/var/log/rexplayer/                   redacted service logs
$XDG_CONFIG_HOME/rexplayer/           per-user settings
$XDG_DATA_HOME/rexplayer/             per-user data
```

`rexplayerd` should expose a narrow authenticated local API and policy-controlled actions rather than handing the UI a Docker socket or unrestricted root shell. Package pre-install checks must verify BinderFS, uinput, evdev, cgroup, namespaces, LSM policy, GPU stack, and supported page size. Unsupported hosts fail closed with a diagnostic report.

## 5. Runtime and isolation contract

The current WSL proof uses a dedicated Docker daemon, `--privileged`, host networking, host IPC, world-writable lab Binder nodes, and ADB on TCP port 5555. Those choices are valid only for the recorded isolated proof and are not a production sandbox.

Before preview distribution:

- replace broad `--privileged` with an enumerated capability/device/seccomp profile;
- avoid host IPC unless a validated Binder namespace design requires it;
- replace host networking with a dedicated network or vsock/local broker;
- keep ADB disabled by default and never expose it to LAN interfaces;
- broker input through a narrow service instead of exposing host device authority to the UI;
- make Binder ownership and cleanup instance-scoped;
- isolate each Android instance's data, ports, IPC, logs, and resource limits;
- enforce CPU, memory, process, and disk quotas;
- test teardown after crashes, host sleep, WSL shutdown, upgrades, and partial installs.

## 6. Graphics, audio, and input packaging boundaries

`/dev/dxg` presence is inventory evidence, not proof that Android rendering uses D3D12 or that frames reach the host UI. Graphics backends must be optional, capability-detected modules with a software fallback and explicit telemetry-free diagnostics.

The input proof verifies raw events through InputReader. Production packaging still needs:

- Windows RawInput and Linux evdev capture with user-visible permission controls;
- application-level Android touch verification;
- coordinate transforms across DPI, rotation, resizing, and multi-monitor layouts;
- focus loss and emergency input release;
- latency and event-loss measurements;
- a per-game/user keymap format with schema migration.

Audio requires a separately gated PipeWire/PulseAudio/WASAPI bridge, device switching, suspend/resume recovery, and latency measurements. None of these modules should be marked installed-and-working solely because prerequisite devices exist.

## 7. Updates and rollback

Use a TUF-style signed metadata chain with root, targets, snapshot, and timestamp roles. The updater downloads into an inactive slot, verifies digest/signature/SBOM policy, and stages without replacing the active runtime.

Recommended sequence:

1. Download to `staging/<release-id>` with resume and size limits.
2. Verify signed metadata, SHA-256, OCI digest, platform, and schema compatibility.
3. Install into immutable versioned directories.
4. Run offline preflight, then a bounded boot/ADB/input smoke test.
5. Atomically switch the `current` pointer only after health passes.
6. Keep at least one last-known-good host/runtime/kernel set.
7. Roll back automatically on startup failure or repeated crash loops.
8. Migrate user data with journaled, reversible schema steps; never couple data deletion to rollback.

Kernel, rootfs, Android image, and host UI may advance independently only when the signed compatibility matrix allows that exact combination.

## 8. Supply-chain and signing requirements

Every promoted release must provide:

- Authenticode-signed Windows binaries/MSI with trusted timestamping;
- signed DEB/RPM repository metadata and packages;
- Cosign-signed OCI images pinned by digest;
- SPDX or CycloneDX SBOMs for host, kernel, rootfs, and Android image;
- SLSA provenance linking source commit, workflow, builder, and artifact digest;
- dependency, container, and license scans;
- reproducible-build comparison where toolchains permit;
- a public checksum file and detached signature;
- documented source offer and notices for redistributed GPL/LGPL/Apache components.

GitHub Actions must pin third-party actions by commit before release promotion. Signing keys are isolated in protected environments; pull-request jobs receive no release credentials.

## 9. CI/CD design

### Pull-request tier

- formatting, lint, unit tests, Rust/C/C++ builds;
- shell and PowerShell parsing;
- evidence semantic validation and checksum verification;
- kernel config/static checks without publishing artifacts;
- SBOM generation and secret scanning;
- no privileged self-hosted execution for untrusted forks.

### Trusted integration tier

- build the pinned WSL kernel from source and compare config/digests;
- build or fetch the pinned Android image and scan it;
- run Linux x86-64/ARM64 substrate tests on dedicated workers;
- run Windows 11 WSL lifecycle tests on disposable, trusted workers;
- verify install, repair, upgrade, rollback, and uninstall;
- archive sanitized evidence and machine-readable gate results.

### Promotion tier

Promote the exact tested digests through `nightly` → `preview` → `stable`; never rebuild during promotion. Stable requires signed artifacts, SBOM/provenance, rollback rehearsal, security review, and all mandatory runtime gates.

## 10. Release channels

| Channel | Audience | Contract |
| --- | --- | --- |
| `nightly` | developers | automated builds; may break; no migration guarantee |
| `lab` | authorized validation hosts | privileged proof tooling and explicit risk warnings |
| `preview` | selected testers | signed installer, supported-host allowlist, rollback support |
| `stable` | general users | only after all production gates pass |

Privileged research helpers belong only in the `lab` channel as separately downloaded artifacts. They are excluded from `preview` and `stable` by policy.

## 11. Mandatory release gates

A Windows or Linux preview is blocked until all applicable gates have repeatable evidence:

1. clean install, repair, upgrade, rollback, and uninstall;
2. no modification of unrelated WSL/container/network state;
3. Android boot and ADB health after reboot, sleep/resume, and crash recovery;
4. application-visible touch with coordinate/rotation/focus tests;
5. rendered frames through the selected GPU path and a bounded software fallback;
6. audio playback/capture and device-switch recovery;
7. ADB and management APIs inaccessible from untrusted network interfaces;
8. least-privilege container/service profile with escape-focused security review;
9. multi-instance data, IPC, port, and quota isolation;
10. measured startup, idle RAM/CPU, input latency, frame pacing, and thermal behavior;
11. signed update, forced-failure rollback, and user-data migration recovery;
12. supported hardware/driver/Windows-build/distro matrix;
13. SBOM, provenance, licenses, signatures, and vulnerability policy;
14. accessibility, diagnostics export, and privacy review.

The 2026-08-20 proof satisfies only part of gates 3 and 4 at the raw input/substrate level.

## 12. Near-term implementation order

1. Keep the existing runtime proof as the immutable feasibility baseline.
2. Build a non-destructive host capability inspector and transaction journal.
3. Decide the Windows kernel isolation path before promising one-click installation.
4. Create a first-party runtime manifest and versioned data layout.
5. Replace proof-wide privileges and host networking with scoped runtime services.
6. Add application-visible input, graphics, audio, lifecycle, and network-isolation gates.
7. Implement signed preview installers and exercise rollback on disposable hosts.
8. Promote only tested digests through release channels.
