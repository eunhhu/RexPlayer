# Integrated-player validation — 2026-10-01

## Scope and baseline

Continuation from user-merged commit `ba8613adc75efcce8e6bbcc5db45288631821194`.
Work is on `codex/integrated-player-20261001`; no merge or main-branch update is
part of this work. This report covers an unsigned native Linux integrated preview,
not a production release or a native Windows GUI/runtime installer.

Implemented source now connects:

- GPUI embedded BGRA frames from explicit-device PNG capture or H264/FFmpeg stream
- Separately consented/supervised scrcpy device-output audio
- Window keyboard/pointer events, schema-v1 keymaps and an asynchronous uinput owner
- Offline host-package stage/healthcheck/atomic activation/rollback/crash recovery
- Verified local Android image copying and an exported administrative review plan

The [runbook](INTEGRATED_PLAYER.md) gives commands and the precise runtime limits.

## Executed source/process checks

Current distinct passing test/doctest counts, excluding repeated aggregate runs:

| Component | Checks passed |
| --- | ---: |
| General Python/evidence/package tests | 20 |
| Runtime inspector, image preparation and release manager | 77 |
| Input core including executable doctest | 20 |
| Linux input transport including compile-only doctest | 17 |
| Launcher contract and actual subprocess tests | 24 |
| Keymap/controller/Linux session/input-worker tests | 24 |
| Media unit and harmless-subprocess tests | 18 |
| Real FFmpeg synthetic-H264 decode and stall/cancel tests | 2 |
| Native-feature UI tests, including startup/image/FIFO | 39 |

Total: **241** distinct test/doctest invocations. The historical Rust proof crate
has zero tests and is not counted. Native UI tests include 32 headless library/
controller/options tests, two native image/FIFO tests, and five compiled-binary
startup tests. Image-object construction does not establish GPU presentation.

Strict native all-target Clippy and Windows-target headless cross-Clippy pass.
The native debug application links. `--help` succeeds; configured-device startup
without a graphical environment exits 69 without contacting that device.

**Final aggregate: 50/50 gates PASS**, including the optimized native GPUI linked
build. Full command evidence is recorded in the delivered
`final-source-checks.json`; see its exact status and command records. This file is
produced by `python3 scripts/check.py --native-ui --json`. It includes strict C
compilation, shell parsing, historical evidence/checksums, Rust format/Clippy/
tests/release builds, actual FFmpeg decoding and PowerShell verification. Live
Android/runtime validation remains explicitly `NOT_RUN` in that report.

## Real filesystem/package behavior

The release-manager suite includes forced process exit during extraction, after
version publication, during healthcheck and on both sides of atomic pointer
replacement. It verifies failure recovery, safe extraction, malformed manifests,
missing-state refusal, corrupt payload refusal and data preservation.

Separately, actual compiled `rex-launcher` packages were installed as versions
`0.2.0-preview.1` and `.2`: install → real executable help → upgrade → executable
help → rollback → executable help passed. The final state retained both releases,
pointed back to `.1`, had generation 3 and preserved a user-data marker. Native
package checks and exact archive hashes are included in the delivered evidence.

The default package healthcheck validates `rex-launcher --help`; it does not boot
Android, render the UI or qualify platform compatibility. Unsigned artifacts
require separate `--allow-unsigned` and `--allow-execution` consent.

## Review and regression fixes

Independent read-only review covered current keymaps/session worker, provisioning,
release transactions, media process/stream ownership and native UI integration.
Concrete fixes included:

- Persistent cleanup poison across restart and repeated-close races
- No silent dropped KeyUp/MouseUp when a status snapshot is contended or a frame
  becomes stale; fail-closed input gating and modifier/layout cleanup
- Nonblocking keymap open followed by regular-file verification, preventing FIFO
  startup hangs
- Owned process-group identity retained until signal delivery; bounded reap and
  cleanup-error propagation even when the decoder cannot start
- External decoder pixel/thread limits in addition to Rust frame-buffer limits
- Missing updater state rejected for an established installation
- Windows cfg/lint gates and inclusion of actual child-process media tests locally

No outstanding concrete code finding remained at the reviewed source checkpoint.
This is not a full security certification or proof of live UI behavior.

## Not run and remaining release requirements

The cloud has no Android, uinput, input, KVM or GPU devices; AF_UNIX socket creation
is denied. No display-server workaround, system security change, hardware write
or privileged runtime installation was attempted.

Therefore these remain **unrun**: visible GPUI window/interaction, actual Android
capture/recording, audible audio, AV synchronization, application-visible touch,
guest device routing, device disconnection/recovery, DPI/rotation on real displays,
latency/frame pacing, sleep/resume, resource use and hardware/driver coverage.
Windows cross-compilation is not Windows runtime execution. See the separate
[read-only Windows assessment](WINDOWS_HOST_CHECK.md).

Known product boundaries also remain: screenshot mode is capped by its polling
interval; screenrecord can hit the device's recording-duration limit; streaming
uses CPU copies; audio has no common video clock; uinput is not automatically
routed to the selected ADB device; unsigned host updates do not install/migrate
Android. Abruptly killing the release-manager process may leave its healthcheck
running, although recovery never silently activates an unconfirmed release.
Signed distribution, runtime isolation/provisioning, native Windows support,
SBOM/license/provenance qualification and the full production gates remain open.
