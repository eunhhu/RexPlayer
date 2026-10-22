# Decision record: media/presentation path

**Status:** proposed, open for review. This document records why the current
ADB-based media path cannot become the product display path, what the alternatives
cost, and which measurements must exist before any latency or game-compatibility
claim is published. It does not implement anything.

## 1. What exists today

`core/media/` is an explicit-device compatibility backend:

- `VideoBackend::Screenshot` — `adb -s <serial> exec-out screencap -p`, one full
  PNG per frame through `bounded_output`, then `decode_png` (`core/media/src/lib.rs`).
  `docs/INTEGRATED_PLAYER.md` states the default interval caps this at **five
  captures per second** and calls it "not a gaming performance target".
- `VideoBackend::Screenrecord` — `adb -s <serial> exec-out screenrecord
  --output-format=h264 -` piped through a bounded local FFmpeg decoder
  (`core/media/src/stream.rs`). CPU copies, device-side encode limits, explicit
  restart on stream end.
- Audio — separately consented `scrcpy --audio-source=output`.

Both video paths require ADB over an already-authorized device serial and are only
exercised by `scripts/check_media_codec.py` against **synthetic** H.264; no live
device measurement exists in CI.

## 2. Why this cannot be the product path

1. **Transport cost.** Every frame crosses adbd twice (guest encode or PNG
   serialisation, host read) instead of staying in GPU memory. PNG capture is
   dominated by encode + transfer, not by decode, and the 5 fps cap is a
   consequence of that design, not a tuning constant.
2. **It contradicts the security roadmap.** `docs/ROADMAP.md` Phase 1 requires
   "Disable ADB by default". The recorded 2026-08-20 substrate shows
   `ro.debuggable=1`, `ro.build.tags=test-keys` and `persist.sys.usb.config=adb`
   as detection signals — the media path depends on the very properties the
   matrix records as failures.
3. **It cannot scale to multi-instance.** Each instance would need its own adbd
   session and its own encode/decode pipeline.

The PNG and `screenrecord` backends stay in the tree as diagnostic and
compatibility paths (useful on locked-down devices where nothing else is
available). They are not the basis for Phase 3.

## 3. Candidate presentation paths

| Path | Mechanism | Cost | Notes |
| --- | --- | --- | --- |
| A. Waydroid surface handoff (Linux first) | Import SurfaceFlinger output as a Wayland/DMA-BUF surface into the host compositor or GPUI viewport (`VK_EXT_external_memory_dma_buf` / EGL image) | Medium-high | Zero-copy, no ADB, no guest encode. Requires Waydroid session ownership rather than "launch and attach". |
| B. Custom micro-VM + virtio-gpu (long term) | Guest renders through virtio-gpu/venus (or gfxstream) into host Vulkan | High | The only path that behaves the same on Windows and Linux; also the only credible multi-instance isolation story. |
| C. WSLg D3D12 (Windows) | Guest or WSLg presents through `/dev/dxg` | Unknown | Presence of `/dev/dxg` is recorded, rendering is not. Must be proven before any Windows GUI scope is promised. |
| D. ADB screenshot / screenrecord (current) | adbd transport | Low | Diagnostic only. ≤5 fps PNG, encode-limited H.264, requires ADB enabled. |

**Proposed decision:** invest Phase 3 in path A on Linux, decide C vs. B for
Windows before any Windows GUI implementation work, and freeze path D at
maintenance.

## 4. Gates that must be measured before any claim

Proposed acceptance criteria for path A (to be approved or changed, not assumed):

- 1080p60 test pattern: input-to-photon p50 ≤ 2 frames (33 ms), p99 ≤ 50 ms.
- 60 s sustained run with zero dropped frames and zero stuck contacts.
- Audio/video drift ≤ 20 ms over 10 minutes with the scrcpy audio path replaced or
  explicitly excluded.
- The measurement harness lives in the repository and is reproducible on the
  documented host matrix; a screenshot of a clock is not evidence.

Until these exist, no document in `docs/` may state a latency, frame-rate or
"zero-copy" number as achieved.

## 5. Supply-chain note (recorded, not resolved here)

`gpui 0.2.2` depends unconditionally on `gpui_http_client` → `zed-reqwest` →
`rustls`/`quinn`, so `core/ui/Cargo.lock` carries 719 packages for a local UI
binary. Stay-current work done in the same change as this document:

- `rustls 0.23.45 → 0.23.46` and `rustls-webpki 0.103.15 → 0.103.16`, clearing
  RUSTSEC-2026-0346, -0347, -0348, -0349 and -0382.
- `deny.toml` added so `cargo deny check advisories bans licenses sources` runs in
  CI for all seven manifests.

Still open: `async-std` (RUSTSEC-2025-0052), `instant`, `paste`,
`proc-macro-error2`, `rustls-pemfile`, `rustybuzz`, `ttf-parser`, `zbus_names`,
`zvariant` are unmaintained and enter only transitively through gpui. They are
allowed by policy (`unmaintained = "workspace"` in `deny.toml`) because they are
not direct choices; removing them requires replacing or forking gpui, which is a
product decision, not a patch.
