# RexPlayer v2: Evidence-Led Roadmap

This roadmap separates completed feasibility evidence from unimplemented product work. Dates are assigned only after the preceding exit gates pass.

## Phase 0 — Feasibility baseline

**Status: completed for the recorded validation substrates.**

- [x] Boot Android 14 with ADB on an ARM64 4 KiB KVM validation guest.
- [x] Build and boot a pinned Microsoft WSL 6.18 custom kernel with BinderFS, uinput, and evdev.
- [x] Boot x86-64 Android 14 through Windows 11 → WSL2.
- [x] Deliver an exact 15-event `/dev/uinput` sequence to Android `getevent` and InputReader `TOUCHSCREEN` classification.
- [x] Record a defensive exposure matrix: `8 PASS / 23 FAIL / 3 SKIP`, `DETECTABLE=YES`.
- [x] Add fail-closed evidence validation, checksums, Windows PowerShell verification, and non-privileged CI gates.

**Boundary:** this phase does not establish a production installer, graphics, audio, application-visible touch, performance, lifecycle recovery, multi-instance isolation, or broad device support.

## Phase 1 — Safe substrate lifecycle

**Goal:** turn the proof into a non-destructive, recoverable runtime transaction.

- [ ] Implement a read-only Windows/Linux capability inspector.
- [ ] Define a signed runtime manifest covering host, kernel, rootfs, Android OCI digest, and schema compatibility.
- [ ] Decide the production Windows isolation path; the proven `.wslconfig` kernel override affects all WSL2 distributions for the user.
- [ ] Provision a dedicated runtime without modifying unrelated WSL distributions, Docker state, firewall policy, or user data.
- [ ] Replace broad proof privileges, host IPC, and host networking with a reviewed minimum policy.
- [ ] Disable ADB by default and expose management only through an authenticated local broker.
- [ ] Add bounded start/stop, crash recovery, sleep/resume, host reboot, and stale-state cleanup tests.
- [ ] Add journaled install/update/rollback/uninstall transactions.

**Exit gate:** 100 consecutive lifecycle runs on disposable supported hosts with no leaked process, mount, network rule, or unrelated host-state modification.

## Phase 2 — Product input and control

**Goal:** move from synthetic proof events to safe user-controlled application input.

- [ ] Implement Windows RawInput and Linux evdev capture behind explicit permission and focus controls.
- [ ] Define a versioned keymap schema and migration tests.
- [ ] Verify Android application-level touch, not only `getevent` and InputReader registration.
- [ ] Test DPI, rotation, resizing, multi-monitor coordinates, focus loss, and emergency input release.
- [ ] Measure end-to-end latency, jitter, event loss, and sustained input under load.
- [ ] Isolate input devices and state per Android instance.

**Exit gate:** a packaged test app confirms correct down/move/up delivery across the supported display matrix with bounded latency and zero stuck-contact failures.

## Phase 3 — Graphics, audio, and host UI

**Goal:** prove a visible, usable Android session before claiming performance.

- [ ] Select and implement the Windows WSLg/D3D12 and Linux Wayland/Mesa presentation paths.
- [ ] Add a software fallback with explicit performance labeling.
- [ ] Verify real rendered frames; `/dev/dxg` or DRM-node presence alone is insufficient.
- [ ] Implement audio playback/capture, device switching, and suspend/resume recovery.
- [ ] Build the unprivileged native host UI and a narrow authenticated runtime-control API.
- [ ] Measure startup, frame pacing, input-to-photon latency, RAM/CPU/GPU use, and thermals.
- [ ] Test multi-instance graphics, audio, input, storage, IPC, network, and quotas.

**Exit gate:** repeatable interactive sessions on the supported hardware matrix with published measurements and recovery tests.

## Phase 4 — Packaging and supply chain

**Goal:** produce signed preview artifacts with atomic updates and rollback.

- [ ] Windows: WiX v4 MSI/bootstrapper with prerequisite inspection, transaction journal, repair, and uninstall.
- [ ] Linux: signed DEB/RPM repositories; keep privileged service policy separate from the UI package.
- [ ] Publish immutable WSL/rootfs/kernel/OCI artifacts through a signed compatibility manifest.
- [ ] Add Authenticode, package signatures, Cosign, SBOMs, license notices, and SLSA provenance.
- [ ] Implement TUF-style update metadata, inactive-slot staging, health checks, atomic activation, and last-known-good rollback.
- [ ] Test interrupted download/install, corrupt artifacts, failed migrations, disk-full conditions, and downgrade policy.
- [ ] Promote exact tested digests through `nightly`, `lab`, `preview`, and `stable` channels.

**Exit gate:** clean install → upgrade → forced failure → automatic rollback → uninstall passes on disposable Windows and Linux hosts without user-data loss.

See [`PACKAGING_AND_RELEASE_STRATEGY.md`](./PACKAGING_AND_RELEASE_STRATEGY.md) for the artifact and release contract.

## Phase 5 — Preview support and stable-readiness

**Goal:** establish a supportable product rather than a one-machine demonstration.

- [ ] Publish the supported Windows build, WSL version, CPU, GPU/driver, and Linux distribution matrix.
- [ ] Add privacy-safe diagnostics export and an explicit opt-in crash-report path.
- [ ] Define security response, vulnerability intake, artifact revocation, and update SLAs.
- [ ] Run accessibility, localization, data-retention, and license reviews.
- [ ] Burn in preview builds and publish known limitations and rollback instructions.
- [ ] Require all mandatory release gates before stable promotion.

## Research tooling policy

The releasable baseline is transparent and detectable. Hidden instrumentation, fingerprint spoofing, integrity bypasses, and anti-cheat evasion are excluded from default packages. Any authorized owned-lab research helper must be a separate opt-in `lab` artifact with explicit risk labeling, independent review, and no undetectability claim.
