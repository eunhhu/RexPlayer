# Integrated native player validation — 2026-10-01

Environment: Debian 13 Linux x86_64, Rust 1.98.1. GPUI pinned to 0.2.2, native X11 + Wayland features. Scope is the integrated compatibility frontend, not production certification.

## Verified in this workspace

- Native `cargo check --features native-gpui`: PASS
- Native all-target strict Clippy (`-D warnings`): PASS
- Native debug binary and test binaries link: PASS
- Windows x86_64 GNU-target headless all-target strict Clippy: PASS (compile-only, no Windows native runtime claim)
- 39 native-feature UI tests: PASS
  - 14 library tests covering original process supervision plus actual-content geometry and logical-key safety plus authoritative event gating/release-loss regressions
  - 13 Waydroid controller/consent/lifecycle tests
  - 5 configuration tests covering explicit serial, backend/timing bounds and prohibited pre-grants
  - 5 executable startup tests (help, missing/empty display, invalid option/timing, no command-line launch consent)
  - 2 native-code tests: real decoded BGRA passed into GPUI RenderImage, and a real FIFO profile rejected immediately without waiting for a writer
- Media fixture suite: 18 ordinary tests pass (plus 2 explicitly run codec tests); bounded actual child process I/O, PNG decoding, malformed/nonzero/stderr/flood/timeout cases, inherited descriptors, cancellation, repeated start, audio command arguments, owned-child reaping, and owner-drop cleanup pass
- Separate real FFmpeg tests: a generated H.264 fixture becomes three correctly sized 64×48 BGRA frames; stalled pipeline cancellation/deadline cleanup pass (these are explicit codec tests, not live Android tests)

The old 22-test shell checkpoint is superseded by the integrated 39-test frontend suite. Core/media and core/keymap have their own additional tests. Release packaging/aggregate results are recorded separately by the repository's integration workflow.

## Reproduce

```sh
cargo fmt --manifest-path core/ui/Cargo.toml -- --check
cargo clippy --manifest-path core/ui/Cargo.toml --locked --all-targets --features native-gpui -- -D warnings
cargo test --manifest-path core/ui/Cargo.toml --locked --features native-gpui
cargo test --manifest-path core/media/Cargo.toml --locked --features test-fixtures
cargo test --manifest-path core/keymap/Cargo.toml --locked --features linux-adapter
```

The native binary's `--help` exits 0. Unset DISPLAY/WAYLAND_DISPLAY exits 69 without starting GPUI or media. The actual GPUI image test verifies dimensions/channel order in its image object; it does not draw pixels or initialize a display.

Native Linux build prerequisites: `build-essential pkg-config libxcb1-dev libxkbcommon-dev libxkbcommon-x11-dev`. Fonts/fontconfig, Wayland/X11 runtime support, Vulkan loader/driver and a usable display are needed for a real window. The cloud has runtime libraries but lacks development linker names; validation used isolated `/tmp/rex-native-link` linker symlinks to those installed libraries via `LIBRARY_PATH`. No system package/settings/device permissions were modified. These cache paths are not part of the distribution.

The declared Rust 1.88 minimum has not been separately compiled; Rust 1.98.1 is the actual validation compiler. Upstream proc-macro-error2 emits a future-incompatibility notice; current application strict Clippy passes.

## Still not verified

- Native window rendering, keyboard/mouse dispatch or accessibility: NOT_RUN. The cloud rejects AF_UNIX socket creation with EPERM, so a display server cannot start. No alternate transport or security-setting workaround was attempted
- Live ADB device capture, Waydroid launch, real Android orientation behavior, secure/DRM frames: NOT_RUN
- Audible playback or A/V synchronization: NOT_RUN. scrcpy process startup alone cannot establish audio
- Actual `/dev/uinput` device creation/writes or Android guest receipt/routing: NOT_RUN. Tests use harmless fake transports; the user must independently provision/verify routing before input enable
- Frame rate, latency, GPU acceleration/zero-copy, hardware compatibility, signing/updaters and cross-platform product support: unverified or unimplemented as applicable
- Forced process termination/crashes cannot guarantee remote Android helper cleanup. The ordinary close path requests bounded owned-worker cleanup and reports deadline failures

Next supported-host gate: render the real native app, select one authorized device, verify both capture backends visually, exercise cancel/repeat/stop/blur/resize/rotation and input failures, verify device-output audio, then measure performance. Treat each capability's evidence independently.
