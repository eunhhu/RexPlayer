# rex-media

Explicit-device Linux compatibility media used by the native GPUI player.
Construction starts idle workers only. Capture and audio each start through an
explicit action; no discovery, pairing, TCP/IP configuration or device selection
is performed automatically.

## Video paths

- `Screenshot`: `adb -s SERIAL exec-out screencap -p`, bounded PNG decoder,
  BGRA frame delivery; default 200 ms interval is at most 5 captures/second
- `Screenrecord`: `adb -s SERIAL exec-out screenrecord --output-format=h264 -`
  feeds a trusted local FFmpeg subprocess directly. FFmpeg emits framed PPM;
  Rust validates dimensions/body length and converts RGB to GPUI BGRA

Frames use a newest-only mailbox. A slow UI does not queue unlimited decoded
frames. Both modes are CPU-copy compatibility paths, not DMA-BUF, zero-copy,
hardware-decoding or low-latency claims. A frame timestamp is host receipt time,
not an Android presentation timestamp. Independent audio has no AV sync clock.

Limits include 8192 per dimension, 16,777,216 total pixels, 16 MiB PNG input,
bounded diagnostic buffers, per-read iteration work and no-frame deadlines.
The streaming decoder also receives `-max_pixels 16777216`, a 64 MiB individual
allocation limit and two decoder/output threads. These are guardrails, not a
whole-process memory/RSS sandbox or an untrusted-code isolation boundary.

Screenrecord support and recording duration are device-dependent. AOSP's default
recording limit is three minutes. End-of-stream requires an explicit restart;
this preview has no invisible reconnect loop. Rotation may not be reflected as a
resolution change by every device. Input geometry must be verified on the host.

## Audio

A separately installed trusted `scrcpy` receives the exact selected serial plus
`--no-video --no-control --no-window --audio-source=output --require-audio
--no-clipboard-autosync`. Android 11+ is required; Android 11 may require an
unlocked screen. scrcpy uploads/runs its temporary server and device-output
forwarding may mute the device speaker. It does not request microphone audio.

`AudioState::ProcessRunning` means the subprocess is alive, not that playback was
heard. Diagnostics/exit are observable. Unsupported scrcpy options fail visibly;
no alternate unsafe fallback is attempted. Audio decoding/playback is delegated
to scrcpy, not reimplemented here.

## Ownership and failure behavior

Screenshot subprocesses and stream process groups have bounded cancellation and
reaping. Stream group leaders remain unreaped with `waitid(WNOWAIT)` until owned
signal delivery is complete, avoiding signaling a reused PID/group ID. The
streaming pipeline tries both cleanups on failure; decoder-spawn failure also
cleans its already-started producer explicitly. Cleanup uncertainty poisons the
session and blocks pending or later starts. scrcpy first receives a graceful
interrupt, then bounded forced cleanup if necessary. No global ADB server or
Android process is signaled.

Ambient remote ADB server routing variables are removed. New local ADB-server
mDNS autoconnection is disabled; an already-running user ADB server remains
outside this module's ownership. Installed executables and the selected device
must be trusted. No frame/screenshot is automatically saved to disk or uploaded.

Call `MediaSession::close` outside the UI event loop for observable completion.
Drop requests cancellation without waiting on the UI thread. Host process
cleanup does not by itself prove guest-side recorder/server teardown.

## Validation

```sh
cargo test --locked --all-features --manifest-path core/media/Cargo.toml
cargo clippy --locked --all-features --all-targets --manifest-path core/media/Cargo.toml -- -D warnings
python3 scripts/check_media_codec.py
```

The last command generates three synthetic H.264 frames with installed FFmpeg,
passes them through the actual decoder pipeline and checks decoded dimensions,
frame count, stalled-pipeline timeout and cancellation. The test fixture feature
also exercises actual harmless subprocess pipes, PNG output, nonzero exits,
output limits, inherited pipes, repeated starts and reaping. There is no attached
Android device, audio output or visible GUI in these checks.

## Primary references

- [Android ADB screenshot/recording documentation](https://developer.android.com/tools/adb#screenrecord)
- [AOSP screenrecord implementation](https://android.googlesource.com/platform/frameworks/av/+/refs/heads/main/cmds/screenrecord/screenrecord.cpp)
- [scrcpy audio options](https://github.com/Genymobile/scrcpy/blob/master/doc/audio.md)
- [FFmpeg codec options](https://ffmpeg.org/ffmpeg-codecs.html)

Checked 2026-10-01. Required real-host media/audio/input gates are in
[the integrated-player runbook](../../docs/INTEGRATED_PLAYER.md).
