# Native shell validation — 2026-10-01

Scope: `core/ui` on Debian 13 Linux x86_64, Rust 1.98.1. This is a checked native shell milestone; **not** a production-readiness or Android-rendering claim.

## Passed

- `cargo fmt --manifest-path core/ui/Cargo.toml -- --check`
- `cargo check --manifest-path core/ui/Cargo.toml --locked --offline --features native-gpui`
- `cargo clippy --manifest-path core/ui/Cargo.toml --locked --offline --all-targets --features native-gpui -- -D warnings`
- Default headless library/controller tests: 17 passed
- Native-feature tests: 22 passed (the same 17 plus 5 executable startup tests)
- Native debug and optimized release binaries compiled and linked against GPUI **0.2.2**, with both X11 and Wayland features
- Full aggregate source checks including native GPUI: 41/41 PASS
- Actual binary `--help`: exit 0 without opening a display or running a diagnostic
- Actual binary with DISPLAY and WAYLAND_DISPLAY unset: exit 69 and explicit no-display error, no GPUI initialization/panic
- Empty display variables, invalid timeout, and attempted command-line pre-approval fail with the expected nonzero codes
- Actual harmless owned subprocesses are reaped and successful/nonzero completion is distinguished

The test suite exercises cancel/repeat actions, one-shot confirmation, stale responses, worker failure, unknown/failed readiness, process-supervision uncertainty, and later request-process failure. It uses no actual Android runtime. Upstream `proc-macro-error2 2.0.1` produces a Rust future-incompatibility notice; current application checks and `-D warnings` pass.

## Linux linker requirements and the verified build

Initial native link failed because the cloud image lacked unversioned development-library linker names:

```text
rust-lld: error: unable to find library -lxcb
rust-lld: error: unable to find library -lxkbcommon
rust-lld: error: unable to find library -lxkbcommon-x11
```

The runtime `.so` libraries existed. A first isolated build-only symlink directory proved the native binary could link. The final validation instead used the official Debian `libxcb1-dev`, `libxkbcommon-dev`, and `libxkbcommon-x11-dev` packages (and matching runtimes), verified against Debian signed package metadata and extracted into a workspace-local sysroot. No packages were installed system-wide, and no services, kernel settings, security settings, or runtime sessions were changed.

On a normally provisioned Ubuntu/Debian build host, these development packages provide the verified missing native linker inputs:

```sh
sudo apt-get install build-essential pkg-config libxcb1-dev libxkbcommon-dev libxkbcommon-x11-dev
```

A native desktop runtime also needs fonts/fontconfig, Wayland/X11 client runtime libraries, a Vulkan loader and a working Vulkan driver. Provisioning common complete build/runtime dependencies on CI may also include `libfontconfig1-dev libwayland-dev libvulkan-dev`. A physical/software Vulkan driver and display server remain separate requirements, not evidence of Android readiness.

Exact successful cloud command environment (paths are validation-only, never committed configuration):

```sh
export PATH=/tmp/rexplayer-validation/cargo/bin:$PATH
export CARGO_HOME=/tmp/rexplayer-validation/cargo
export RUSTUP_HOME=/tmp/rexplayer-validation/rustup
export LIBRARY_PATH=/workspace/shared/rex-ui-validation/root/usr/lib/x86_64-linux-gnu
cargo build --manifest-path core/ui/Cargo.toml --locked --offline --features native-gpui
cargo build --manifest-path core/ui/Cargo.toml --locked --offline --features native-gpui --release -j 2
cargo test --manifest-path core/ui/Cargo.toml --locked --offline --features native-gpui
```

The workspace sysroot is not part of the repository or a distribution artifact. Standard installed development packages should be used on supported build hosts. GPUI and its dependency graph must be downloaded before `--offline` runs; no offline-first bootstrap claim is made.

## Not run / blocked

- **Native window rendering and interaction:** NOT_RUN. Xvfb and Mesa lavapipe were obtained from official Debian packages for a software-rendered shell attempt, but this execution sandbox rejects local AF_UNIX socket creation with `EPERM`, preventing the display server from starting. Rendering attempts stopped at this explicit restriction. No screenshot or visual QA is claimed
- Live Waydroid/Android session, actual app-visible input, audio, embedded frames, frame timing, GPU performance, or hardware compatibility: NOT_RUN; this host lacks the runtime/devices and working native display
- Native Wayland rendering, Linux aarch64 compilation, Rust 1.88 minimum-version compilation, signing, installers, updates and distribution-license review: NOT_RUN
- Windows/macOS runtime support: not implemented by this shell or the launcher

The five native startup tests execute the linked binary but do not draw a window. Headless controller tests do not validate GPUI event dispatch or pixels. The next supported-host gate is to render the real shell, exercise keyboard/mouse confirmation and cancellation repeatedly, and separately validate a pre-provisioned same-user/display Waydroid runtime without treating process exit as rendered-frame evidence.
