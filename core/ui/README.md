# RexPlayer native shell milestone

A real Rust/GPUI 0.2.2 desktop shell for the existing `rex-launcher` library. It offers read-only Doctor and Status, a **per-request confirmation** before opening Waydroid's separate Android UI, asynchronous loading/error results, keyboard shortcuts, and owned request-process exit reporting.

This is an opt-in product milestone, not a production Android distribution. It does not embed Android frames, implement audio/app-visible input, provision Android, or verify rendering merely from a successful process spawn. Linux x86_64/aarch64 only; the underlying Waydroid runtime must already be independently provisioned and running as the same ordinary user on the same Wayland display.

## Build

Rust 1.88+ is declared; validation uses Rust 1.98.1. GPUI is pinned to the published `=0.2.2` crate, with its locked dependency graph. The GUI is behind `native-gpui` so hardware-independent library checks and the existing CLI remain independently usable.

```sh
# Pure controller/process tests, no graphical session required.
cargo test --manifest-path core/ui/Cargo.toml --locked
cargo clippy --manifest-path core/ui/Cargo.toml --locked --all-targets -- -D warnings

# Actual native code check and native binary.
cargo check --manifest-path core/ui/Cargo.toml --locked --features native-gpui
cargo clippy --manifest-path core/ui/Cargo.toml --locked --all-targets --features native-gpui -- -D warnings
cargo build --manifest-path core/ui/Cargo.toml --locked --features native-gpui
core/ui/target/debug/rex-player --help
core/ui/target/debug/rex-player
```

Use `--waydroid /absolute/path/to/trusted/waydroid` and `--timeout-ms 100..30000` when needed. Options are validated by `rex-launcher::Options`; this shell does not duplicate its argument parser or status parser. The executable must be trusted; it is not sandboxed. Runtime checks refuse privileged/root operation. There is no CLI option to pre-grant the UI confirmation.

The native Linux build uses GPUI's Wayland and X11 backends and requires their system build/runtime dependencies (including fontconfig, xkbcommon, X11, Wayland, Vulkan). A working graphical session and compatible Vulkan device/driver are required to show the shell. X11 can render the shell, but Waydroid readiness still requires the matching Wayland environment. A missing display returns code 69 before GPUI initialization; existing but unusable display variables do not prove a usable display. Window-creation errors return 70. A successful application exit proves neither Android readiness nor rendered frames.

## Actions and consent

- Opening the shell performs one read-only Doctor check
- Doctor establishes a point-in-time readiness result through `rex-launcher`
- Status is read-only and never establishes UI readiness by itself
- Opening the confirmation view runs no runtime command
- Cancel/Escape dismisses that view without a command or stored permission
- “Allow once & open” acknowledges exactly the upstream side effects described in `rex-launcher launch --allow-session-start`
- After consent, the launcher checks the current runtime again and refuses stopped/frozen, foreign-user/display, malformed, unknown, or otherwise unconfirmed state
- Every subsequent launch requires a fresh Doctor result and another confirmation
- Busy actions and repeated launch/confirm clicks are rejected by the controller; at most one owned UI-request process can be active

Shortcuts: Ctrl-R Doctor, Ctrl-S Status, Ctrl-L open confirmation, Ctrl-Enter confirm the shown prompt, Escape cancel, Ctrl-Q close. Confirmation cannot be triggered by Ctrl-Enter when no prompt is shown. Native assistive-technology behavior remains unverified; keyboard shortcuts are not a claim of full accessibility support.

## Asynchrony and process ownership

The GPUI render/event path does no backend process I/O, sleeps, or blocking waits. A dedicated worker uses a bounded, nonblocking request mailbox and the existing launcher's bounded diagnostic backend. The window polls ready events with a GPUI asynchronous timer and ignores stale completions. Diagnostic timeouts/output limits remain those of `rex-launcher`.

The CLI's spawn-and-drop backend is intentionally **not** reused for UI spawning. The shell retains the `show-full-ui` child handle, polls its exit without blocking the UI, and reaps it. It distinguishes request-spawn success from later exit status and from real rendered frames. If waiting fails, additional UI requests remain blocked. Closing the view sends no stop signal: while the host process lives, the worker retains/reaps any existing request child; when the whole application exits, normal OS reparenting applies. It never kills a session/container, signals by process name/group, installs services, or manages Android lifecycle. The app does not claim termination-signal cleanup it cannot guarantee.

A process that blocks indefinitely can keep the single-request guard active indefinitely. The user owns the runtime; this milestone deliberately does not infer permission to kill or recover it.

## Verification

The checked-in headless tests exercise readiness, one-shot consent, cancel/repeat actions, stale completions, error recovery, read-only/launch separation, missing runtime, launch-without-consent, and reaping actual harmless subprocesses. They do not stand in for Android or hardware testing.

See [VALIDATION.md](VALIDATION.md) for native compiler/linker results, the 22 passing tests, and the explicit sandbox restriction preventing native rendering. Rendering, Android launch, embedded graphics, audio, input, performance, signing/packaging/updates, and real hardware compatibility each require their own evidence. No mock or software-rendered shell can establish those runtime capabilities.

## Primary API references

Implementation reviewed against the pinned published source, rather than moving-main API assumptions:

- [GPUI 0.2.2 hello-world example](https://docs.rs/crate/gpui/0.2.2/source/examples/hello_world.rs)
- [GPUI 0.2.2 window-close example](https://docs.rs/crate/gpui/0.2.2/source/examples/on_window_close_quit.rs)
- [GPUI 0.2.2 Context API](https://docs.rs/gpui/0.2.2/gpui/struct.Context.html)
- [GPUI official site](https://gpui.rs/)

GPUI is Apache-2.0, independently licensed from RexPlayer. Shipping binaries needs a complete dependency/license review and notices. An upstream `proc-macro-error2` future-incompatibility notice is present under the validation toolchain; it is not an application warning or a failed current check.
