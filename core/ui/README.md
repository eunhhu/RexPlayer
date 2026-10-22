# RexPlayer integrated native player

A real Rust/GPUI 0.2.2 window integrating `rex-media`, `rex-keymap`, `rex-input-linux`, and the existing `rex-launcher`. Linux x86_64/aarch64 only. This is a functional compatibility implementation with explicit supported-host validation still required, **not a production-ready or measured low-latency release**.

## Build and launch

Rust 1.88+ is declared; current validation uses 1.98.1. Linux native linking needs `libxcb1-dev libxkbcommon-dev libxkbcommon-x11-dev`; a real window also needs fonts, a graphical session, and a compatible Vulkan driver. GPUI is pinned to `=0.2.2` with a checked-in lockfile.

```sh
cargo test --manifest-path core/ui/Cargo.toml --locked
cargo clippy --manifest-path core/ui/Cargo.toml --locked --all-targets --features native-gpui -- -D warnings
cargo test --manifest-path core/ui/Cargo.toml --locked --features native-gpui
cargo build --manifest-path core/ui/Cargo.toml --locked --features native-gpui
core/ui/target/debug/rex-player --help
```

The `native-gpui` feature keeps headless controller/option/geometry tests independent of GPU/system linker dependencies. On a fresh machine, download dependencies before adding `--offline`. The shell rejects root or an unknown effective UID and refuses missing display variables before initializing GPUI. Merely setting DISPLAY/WAYLAND_DISPLAY does not prove a usable display.

### Selected-device video

Connect and authorize a device yourself first using official Android tooling. Copy its exact serial. RexPlayer never picks the first device, runs discovery/pairing/TCP setup, or changes connection settings.

```sh
# Compatibility fallback: bounded PNG screenshots, default maximum 5 polls/sec.
rex-player --adb-serial YOUR_SERIAL

# Continuous H.264 compatibility stream decoded by an existing trusted FFmpeg.
rex-player --adb-serial YOUR_SERIAL --video-backend screenrecord --ffmpeg /usr/bin/ffmpeg

# Optional executable/profile/timing configuration.
rex-player --adb-serial YOUR_SERIAL --adb /usr/bin/adb --scrcpy /usr/bin/scrcpy \
  --capture-interval-ms 200 --capture-timeout-ms 3000 \
  --keymap core/keymap/examples/default.json
```

Configuration alone does not capture, play audio, or create an input device. Choose **Start capture** in the window. The selected serial is displayed beside the controls. ADB may start its local server; ambient remote-server routing variables are removed and new-server mDNS autoconnection is disabled. A separately existing ADB server remains user-owned.

- Screenshot mode executes `adb -s SERIAL exec-out screencap -p`, validates bounded PNG dimensions/bytes, and decodes to BGRA off the UI thread
- Screenrecord mode executes the fixed Android H.264 command and supervises a local FFmpeg decoder; complete bounded PPM frames become BGRA. Screenrecord has device/version-dependent duration and rotation limits. EOF, failures and stalls stop capture and require an explicit restart
- `--stream-stall-ms 500..30000` controls the stream's no-frame deadline, default 5000 ms
- No CPU-copy backend claims zero-copy, target FPS, measured latency, A/V synchronization, or secure/DRM-content support

A newest-frame-only mailbox prevents an accumulating image queue. GPUI receives an owned decoded buffer through `RenderImage`, displays it with centered aspect-preserving containment, and explicitly evicts replaced images from its texture cache. Frame/decoder evidence does not assert successful GPU presentation.

### Explicit audio

**Start audio…** opens a separate confirmation explaining that scrcpy uploads/runs its temporary Android server and may mute the device speaker while forwarding device-output audio. Only confirmation starts the selected-device audio process. Microphone capture, video, clipboard autosync and remote control are disabled. A separately installed compatible official scrcpy and Android 11+ are needed. Process-running status never claims audible playback.

**Stop audio** and **Stop player** request asynchronous cleanup. “Stopping” remains visible until owned-process cleanup completes. The app never sends global ADB/session/container stop commands. See `core/media/README.md` for subprocess ownership limits, including scrcpy's own helper processes and exceptional cleanup deadlines.

### Explicit mapped input

The keymap is loaded and validated before the window opens, from a bounded regular JSON file. Opening a FIFO is nonblocking and rejected. Merely loading a profile creates no device.

**Enable mapped input…** is available only with a fresh frame and settled prior input cleanup. Confirmation warns that `/dev/uinput` creates a **host virtual touchscreen**, not a transport automatically bound to the chosen ADB serial. Existing guest routing must already be independently configured and verified. Otherwise input can affect the host desktop. The app never changes device permissions or guest routing.

After explicit enable, the keymap worker owns device creation, input writes, and cleanup. GPUI forwards only this active window's events, with actual measured content bounds excluding letterboxing and decoded Android dimensions. Key repeats are ignored. Input is disabled on:

- Escape/emergency release, explicit stop, opening a confirmation, or closing the window
- Focus loss, resize/move, changed content bounds, or changed frame dimensions/orientation
- Capture failure or no fresh decoded frame for two seconds
- Queue overflow, translation/transport errors, keyboard-layout changes, or modifier changes

Cleanup is requested out-of-band and pending source events are discarded. The prior worker remains owned until completion is observed; there is no automatic device recreation or re-enable. A shutdown deadline is reported as unverified cleanup, never successful release delivery.

GPUI 0.2.2 provides logical keys whose release symbols can change with modifiers/layout. This frontend therefore **explicitly rejects profile bindings for Shift, Control, Alt and Escape** and disables mapped input when modifiers/layout change. Use unmodified keys. Unknown/mismatched releases with held keys trigger fail-closed cleanup; the core keymap's broader vocabulary does not imply native support for every modifier binding. Global input hooks, pointer locking, IME/scancode remapping, and accessibility behavior are not implemented or verified.

### Optional Waydroid controls

Doctor/Status and the separate-window launcher remain available and still reuse `rex-launcher`'s parser/policy. Without `--adb-serial`, opening the shell performs the original read-only Doctor check. With a serial, media can work independently of Waydroid installation; its optional Doctor is not an ADB capture gate.

Every Waydroid UI request requires a new confirmation acknowledging the same session-start/unfreeze effects as `--allow-session-start`. Confirmation rechecks readiness. The shell retains/reaps owned UI-request processes and distinguishes spawn from later exit and rendered frames. It never intentionally stops Android when closed.

Shortcuts: Escape release/cancel, Ctrl-Q close, Ctrl-R Doctor, Ctrl-S Status, Ctrl-L Waydroid launch prompt, Ctrl-Enter confirm that prompt. Audio/input permission cannot be supplied through CLI flags.

## Validation and limitations

See [VALIDATION.md](VALIDATION.md). The integrated native code compiles and links, and its startup/image conversion/profile loading/controller tests run without a display. The current cloud's AF_UNIX restriction prevents a native display server, so no actual native pixels, event dispatch, Android connection, speaker output, live uinput writes, FPS, latency, or guest routing have been verified here. No workaround bypasses that restriction.

GPUI's primary pinned references: [hello-world](https://docs.rs/crate/gpui/0.2.2/source/examples/hello_world.rs), [Context](https://docs.rs/gpui/0.2.2/gpui/struct.Context.html), [image element](https://docs.rs/crate/gpui/0.2.2/source/src/elements/img.rs), and [official project](https://gpui.rs/). GPUI is Apache-2.0; packaging all dependencies requires the applicable licenses/notices. External ADB/scrcpy/FFmpeg remain separately provisioned trusted dependencies.
