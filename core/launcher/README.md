# Native launcher milestone

A dependency-free Rust CLI named `rex-launcher` for **pre-provisioned Linux x86_64/aarch64 Waydroid only**. It is an unprivileged command-line entry point and policy backend for the separate `core/ui` GPUI shell, not a production Android runtime distribution.

## Build and non-privileged checks

Requires Rust 1.82+ and Cargo. There are no third-party crates.

```sh
cargo fmt --manifest-path core/launcher/Cargo.toml -- --check
cargo clippy --manifest-path core/launcher/Cargo.toml --locked --offline --all-targets -- -D warnings
cargo test --manifest-path core/launcher/Cargo.toml --locked --offline
cargo build --manifest-path core/launcher/Cargo.toml --locked --offline --release
core/launcher/target/release/rex-launcher --help
```

Tests use an injected backend and separate processes of the test harness. They do not require installed Waydroid, Android images, device access, root, D-Bus, or a graphical desktop. Four ignored helper tests are invoked explicitly as subprocess fixtures by the ordinary tests; their timeout, output-limit, nonzero-exit, and pipe behavior is covered.

## Commands

```sh
rex-launcher doctor
rex-launcher status
rex-launcher status --waydroid /usr/bin/waydroid --timeout-ms 5000
```

`status` reads the existing Waydroid state. A stopped but recognized status is a successful read, not a launch-readiness assertion. `doctor` additionally requires both session and container to be running, the session's numeric UID to match the current effective UID, matching `WAYLAND_DISPLAY`, and an absolute `XDG_RUNTIME_DIR`.

Only `launch` can request the Android UI, and it requires explicit acknowledgement:

```sh
rex-launcher launch --allow-session-start
```

**Why the flag matters:** upstream `waydroid show-full-ui` can unfreeze a container or start a session if its D-Bus session check fails. Even a successful status check is not atomic with this later operation. `--allow-session-start` acknowledges those upstream side effects. It is required every invocation; no permission is saved. Without it, launch executes nothing. With it, the launcher still refuses stopped/frozen, foreign-user, foreign-display, malformed, or otherwise unconfirmed state. It never directly invokes `session start`, `session stop`, `container`, `init`, `upgrade`, `sudo`, or a shell.

`--waydroid` selects an existing **trusted executable**. It is not a sandbox or an installer; do not supply an untrusted program. The default resolves `waydroid` using the caller's PATH. No credentials or privileges are acquired. Runtime commands reject root and fail if the effective UID cannot be read from `/proc/self/status`. Windows, macOS, and other Linux architectures fail explicitly as unsupported; help still works. Linux nonblocking-pipe constants are deliberately restricted to the x86_64/aarch64 ABI.

## Conservative diagnostic contract

The adapter invokes exactly `waydroid status` with C locale. It requires a zero exit, empty stderr, valid UTF-8, known fields, nonduplicate state fields, and the expected upstream status layout. Upstream may return zero for an uninitialized runtime, so exit status alone never establishes readiness. The reviewed upstream implementation prints plain text without ANSI colors. ANSI escape sequences, additional log lines, and unknown future formats deliberately fail closed until reviewed; they are not silently stripped into trusted status.

Diagnostics have a default 3-second deadline (configurable 100–30000 ms) and a 64 KiB limit **per output stream**. Linux nonblocking pipes avoid hangs from full stdout/stderr or inherited descriptors. On timeout/output/error, only the directly spawned diagnostic child is killed, with up to 250 ms for reaping. It does not kill descendant process groups or any externally owned session/service. Kernel-level process creation or termination stalls are outside the userspace deadline guarantee; a child stuck in the kernel may not be reaped in that cleanup interval.

Exit codes:

- `0`: successful status read, doctor readiness check, or UI-request **spawn**; inspect the text for which result
- `3`: recognized runtime/host is not ready for UI integration
- `64`: unsupported command, invalid option, or argument error
- `69`: unsupported OS, privileged/unknown user context, or missing diagnostic executable
- `70`: diagnostic failure, unknown output, excessive output, or UI spawn failure
- `75`: diagnostic timeout
- `78`: launch acknowledgement missing

## Process and signal ownership

The short-lived CLI owns only its read-only diagnostic child. It uses no shell, process-name kill, process-group signal, daemon management, or session teardown. Normal diagnostic completion is waited/reaped; deadline/error cleanup targets only that child. It does not install signal handlers. If the launcher itself is externally terminated, the OS's normal signal/process behavior applies; an externally killed launcher cannot promise child cleanup. Diagnostic commands receive closed stdin.

An explicitly requested UI command is spawned with closed stdin and inherited stdout/stderr. The launcher prints its PID and exits without waiting for completion or killing it. The user/OS owns that process and the existing Waydroid session. Later UI-command failures may appear on the inherited terminal; the launcher's zero exit means **spawn succeeded only**. No later exit status, D-Bus readiness, visible window, Android frame, application input, GPU acceleration, or audio is asserted. Closing/killing the launcher never intentionally stops Waydroid.

This library's asynchronous UI method is intended for this short-lived CLI. The separate `core/ui` GPUI shell uses a supervising backend that retains and reaps its owned UI-request child rather than this short-lived method. Rendered frames still require separate runtime validation.

## Verified upstream contract

Reviewed 2026-10-01 against primary sources:

- [Official Waydroid command-line documentation](https://docs.waydro.id/usage/waydroid-command-line-options): `status` and `show-full-ui`
- [Upstream status implementation](https://github.com/waydroid/waydroid/blob/main/tools/actions/status.py): session/container, numeric session user, and Wayland display fields
- [Upstream app-manager implementation](https://github.com/waydroid/waydroid/blob/main/tools/actions/app_manager.py): `showFullUI` calls `maybeLaunchLater`, whose fallback may start/unfreeze a session

These upstream URLs are moving references, not a pinned or tested Waydroid binary version. The parser and policy are intentionally conservative; production support requires a versioned backend compatibility contract and real supported-host validation.

## Still unverified or unimplemented

No live Waydroid launch or rendered Android session was tested in the development container. It lacks the Android runtime and required device/display environment. The separate GPUI shell compiles and has controller/startup tests, but its actual window remains unverified. Native Windows launch, provisioning, lifecycle recovery, signed packaging/updates, display embedding, audio, app-visible input, performance, and hardware compatibility remain future work. The current milestone provides actual CLI code and fail-closed host/backend checks without claiming those product capabilities.
