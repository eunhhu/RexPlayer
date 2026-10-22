# Integrated Linux player preview

This source milestone connects the previously separate native shell, media,
keymaps and host-package lifecycle. It is not production qualification.

## Run from source

On a supported native Linux graphical desktop, with Rust and GPUI build libraries:

```sh
cargo build --locked --release --manifest-path core/ui/Cargo.toml --features native-gpui
core/ui/target/release/rex-player --help
core/ui/target/release/rex-player --adb-serial YOUR_ALREADY_AUTHORIZED_DEVICE
```

Select the exact existing ADB device serial. The application does not auto-select,
pair, connect, enable TCP/IP, modify firewall rules or provision Android. Trusted
`adb`, `scrcpy` and optionally `ffmpeg` must be installed separately. The Waydroid
Doctor/Status/Launch controls remain available independently of selected-device
media; no Waydroid installation is needed to configure the ADB compatibility path.

Start capture explicitly in the window. The default screenshot mode polls Android
PNG screenshots with bounded subprocess deadlines and newest-frame-only buffering.
At the default interval it is capped at five captures per second, not a gaming
performance target. For H.264 streaming into the same embedded GPUI viewport:

```sh
core/ui/target/release/rex-player --adb-serial YOUR_ALREADY_AUTHORIZED_DEVICE \
  --video-backend screenrecord --ffmpeg /path/to/trusted/ffmpeg
```

Streaming supervises Android `screenrecord` and a local FFmpeg decoder. It uses
CPU copies and has bounded frame dimensions, decoder allocation/pixel settings,
thread counts, diagnostic output, no-frame timeout and process-group cleanup.
Device recording limits can end the stream; restart is explicit. Protected content,
codec support, orientation and device permission errors remain device-dependent.
Local synthetic H.264 tests are evidence for the decoder and process supervision,
not for a live Android encoder or real-time performance.

Audio is a separate explicit consent action using trusted scrcpy audio-only mode.
It uploads/runs scrcpy's temporary server and may mute the device speaker during
forwarding. Microphone capture and scrcpy remote control are disabled. A running
process does not establish audible output. Video/audio clock synchronization,
audio device switching and latency still require real-host qualification.

## Input

The default versioned profile and format are documented in
[`core/keymap`](../core/keymap/README.md). Pass `--keymap /path/to/profile.json` to
load a bounded, validated custom profile. Tap, hold, WASD joystick and direct
pointer mappings are window-scoped. There is no global keyboard hook or evasion.

Creating the uinput device requires a separate in-window opt-in and existing
permissions. **A selected ADB device is not a uinput route.** Verify that the intended
local Android guest receives this exact virtual device before enabling input;
uinput cannot control an arbitrary USB/network ADB device. The application changes
no device permissions or container bindings. Focus loss, Escape, geometry change,
stale video and failure stop input and release or destroy the owned input session;
actual Android cancellation/delivery remains a mandatory host test.

## Install, update and rollback

```sh
python3 scripts/package_preview.py --native-ui --version 0.2.0-preview.1 \
  --output /path/to/new-rexplayer.tar.gz
python3 runtime/release_manager.py --root /path/to/private-releases install \
  --archive /path/to/new-rexplayer.tar.gz \
  --manifest /path/to/new-rexplayer.tar.gz.manifest.json \
  --allow-unsigned --allow-execution
python3 runtime/release_manager.py --root /path/to/private-releases \
  run --allow-execution player -- --adb-serial YOUR_ALREADY_AUTHORIZED_DEVICE
```

The two consent flags matter: these are unsigned local packages, and activation
runs their executable healthcheck. The default healthcheck exercises launcher
`--help`, **not Android boot or GUI rendering**. Read
[release transactions](../runtime/RELEASE_MANAGER.md) before choosing this preview.
Android image preparation has a separate [non-privileged review-plan tool](../runtime/PROVISIONING.md);
it does not run administrative initialization or install a runtime.

## Required host gates

On the intended supported machine, verify all of the following before promotion:

1. Render actual Android content, resize/DPI/rotation and protected-content failure
2. Start/stop/restart both capture backends, unplug/unauthorize device, close during
   startup and active streaming, verify no leftover owned subprocesses
3. Confirm audio playback, consent/cancel, device changes and audio/video timing
4. Route the exact virtual input device to the intended guest and verify app-level
   down/move/up, simultaneous keys, focus/Escape/resize/stale-frame cleanup
5. Test installation/upgrade/forced health failure/rollback using the actual native
   package and retain user data across versions
6. Qualify supported distributions/drivers, sign releases and supply dependency
   notices/SBOM/provenance; select the Windows isolation strategy before shipping
   a Windows privileged installer

The cloud sandbox has no Android/uinput/GPU devices and denies AF_UNIX socket
creation, so this iteration cannot produce a visible window or live Android,
audio or touch result. No sandbox workaround or host security change is attempted.
