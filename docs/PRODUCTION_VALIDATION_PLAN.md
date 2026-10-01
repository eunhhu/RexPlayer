# Production validation plan and current delivery boundary

This branch begins v2 product components; it is **not a production-ready Android player**.
The historical `proof/evidence/` files remain unchanged and record the August 2026 lab runs.
Revalidating their checksums is not a fresh Android boot, rendering, or input test.

## What can run without privileged host access

Run the complete aggregate on Linux with Python 3, GCC, Bash, Rust/Cargo with
Clippy/rustfmt, and PowerShell (`pwsh`) available. Missing tools are BLOCKED,
not passed. Windows CI runs the portable Python/Rust component subset; the
Linux C proofs and live Android checks are separate.

- `python3 runtime/capability_inspector.py`: conservative, read-only capability inventory
- `cargo test --locked --manifest-path core/input/Cargo.toml`: native touch-state logic
- `cargo run --locked --manifest-path core/launcher/Cargo.toml -- --help`: native launcher entrypoint
- `cargo test --locked --manifest-path core/input-linux/Cargo.toml`: Linux transport/frame and failure-state tests
- `cargo run --locked --manifest-path core/ui/Cargo.toml --features native-gpui -- --help`: linked GPUI shell entrypoint
- `python3 scripts/check.py`: aggregate headless/source checks with PASS/FAIL/BLOCKED outcomes
- `python3 scripts/check.py --native-ui`: also checks, tests and links the optimized GPUI binary
- `python3 scripts/check.py --json > source-checks.json`: machine-readable check report
- `python3 scripts/package_preview.py --output /tmp/rexplayer-preview.tar.gz`: unsigned Linux launcher package
- Add `--native-ui` to that packaging command to include the optimized GPUI shell

The launcher requires an existing, configured Linux Waydroid environment. It does not
supply Android, provision a container, install a kernel, or implement a GPUI viewport.
The input core has an explicit Linux uinput adapter, but host input capture, keymaps
and actual Android application delivery are not yet integrated/verified. The native
GPUI shell is compiled and startup-tested; it does not embed the Android display.
This cloud denies AF_UNIX socket creation (EPERM), blocking an Xvfb/Wayland display
even though Mesa llvmpipe software Vulkan works. No native screenshot or UI-click
coverage is claimed.
The preview archive is neither an installer nor a stable release; its checksums are
integrity metadata, not publisher signatures or trust attestations.

## Environment needed to finish live validation

Use an explicitly authorized dedicated/disposable Linux or Windows 11 lab machine.
Do not repurpose unrelated WSL distributions or production Docker workloads. Start
with the new read-only inspector and preserve its JSON report with OS, kernel, tool
versions, and exact tested source revision. Record unknown and inaccessible probes
as such. Do not convert a missing device or skipped test into PASS.

### Native Linux application gate

1. Use a supported graphical Wayland desktop with a working, pre-provisioned Waydroid
   Android session. Confirm the owner, session and container state using `waydroid status`.
2. Run launcher `doctor` and `status`; retain output and exit status. Missing prerequisites
   must fail clearly and must not silently provision or modify the host.
3. Review the launcher README before `launch`. Upstream `show-full-ui` can start or
   unfreeze the user's session after the status check; the launcher requires explicit
   `--allow-session-start` acknowledgement for that behavior.
4. Observe actual Android frames and interact with an owned test application. Record
   screenshots/video and application-level MotionEvent events. A process spawn, an
   existing device node, or `getevent` output alone is not sufficient.
5. Exercise repeated launch, close/reopen, backend exit, display disappearance, focus
   loss, and denied/missing permissions. Confirm unrelated processes/sessions remain intact.
6. Measure frame timing, input-to-photon latency, CPU/RAM/GPU use and startup on named
   hardware. No numeric performance promise is made before those measurements.

### Android input gate

The existing `proof/input/` runner creates virtual input and can require privileges;
read its lab-host warnings first. It is not an ordinary source-test step. On a permitted
host, preserve producer/getevent/InputReader outputs and exit codes. Then use an owned
Android test app to verify down/move/up, simultaneous contacts, rotation, resize, DPI,
focus loss, emergency release, and uninterrupted input under load. Test 100 lifecycle
iterations and verify no stuck contact, leaked device or mount after cleanup.

### Windows gate

The existing `proof/windows-wsl/README.md` describes the recorded validation-only
custom-kernel path. Applying `.wslconfig` affects all the user's WSL2 distributions;
this is an unresolved product isolation decision, not an installer default. Obtain
explicit authorization before security-sensitive host/kernel settings, privileged
container startup, or persistent access changes. Validate Windows-native PowerShell,
WSL boot, Android application display/audio/input, suspend/resume, cleanup and rollback
on the chosen host. Linux PowerShell execution is not Windows-native verification.

## Remaining product stages

1. **Runtime ownership and isolation:** decide Windows isolation boundary; implement a
   dedicated runtime lifecycle, authenticated local broker, bounded start/stop/recovery,
   and least-privilege policy without affecting existing workloads
2. **Input integration:** versioned keymaps and persistence; platform input adapters;
   connect the tested core to actual Android touch; focus/permission controls and app tests
3. **Native GPUI and media:** verify the implemented shell, integrate the rendered Android surface,
   resizing, audio/device switching, and recovery; verify real frames on both platforms
4. **Transactional delivery:** signed runtime compatibility manifests, installer/update/
   rollback/uninstall journal, artifact verification, privacy-safe diagnostics and SBOM
5. **Release qualification:** supported-hardware matrix, 100-run lifecycle and media/input
   endurance, clean-install/upgrade/interruption/rollback/uninstall testing, accessibility,
   license review, and reproducible measured resource budgets

No merge or deployment is part of this branch. A source-check pass or unsigned preview
package must never promote the project to production-ready or substitute for these gates.
