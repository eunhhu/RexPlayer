# rex-input-linux

A real, explicit Linux `/dev/uinput` transport for the typed transitions in
[`rex-input-core`](../input). It creates one recognizable virtual touchscreen,
validates every transition, and submits Linux type-B multitouch frames through
pinned `evdev 0.13.2`. It does not capture host input, grab physical devices, install
hooks, adjust permissions, load kernel modules, run privileged commands, create a
Waydroid route, or implement keymaps or integrity/anti-cheat evasion.

**This component is not an integrated production input path.** Device creation,
kernel event delivery, Android classification, app-visible gestures, latency, and
failure behavior on actual Linux/WSL hardware still require live validation. The
old `proof/input-rust` results do not validate this new adapter.

## Integration

The crate's API exists on Linux only. Add it under a Linux target-specific
dependency when using it in a cross-platform application. Construct a
`DeviceConfig` with the same `AndroidSize` and slot count as the source allocator.
`DeviceConfig::new` performs no I/O. `LinuxTouchDevice::create(config)` is the only
real-device constructor; it opens `/dev/uinput` and returns creation/permission
errors directly. There is no automatic start, fallback, or reconnection.

See the compile-checked `no_run` example in `src/lib.rs`. Applications must
coordinate display dimensions, orientation, input focus, device discovery, and
consumer readiness themselves. `device_nodes()` lists only this virtual device's
nodes and does not open them; discovery may initially return an empty list.
Creating this device on a desktop can make its events visible to the desktop, so
live tests belong in a disposable, isolated test environment with explicit routing.

- `submit(&[ContactTransition])` validates the entire frame before writing anything
- Each frame may change a slot once; split a contact's down/move/up into separate
  frames and preserve the source allocator's order
- A frame cannot exceed the configured slot count; empty frames perform no I/O
- Coordinates and tracking IDs must fit the advertised capability ranges
- Downs require free slots, unique live caller identities, and strictly increasing
  tracking IDs; moves and releases must match the live identity and tracking ID
- `Up` positions must match the last successfully submitted position, preventing
  stale snapshots from silently releasing a different state
- `BTN_TOUCH` appears first and describes whether any contacts remain after the
  frame; slot selection is explicit and releases use tracking ID `-1`
- `ABS_X/Y` follows the oldest remaining contact; the primary changes when needed
- The `evdev` transport appends exactly one `SYN_REPORT` after the event body
- Focus loss: stop the source stream first, then submit the full batch returned by
  `ContactAllocator::focus_lost()`; repeated empty cleanup performs no writes
- Alternatively, `release_all()` releases all adapter-known contacts. Clear the
  source allocator too and discard its release batch instead of submitting twice
- `close()` consumes the adapter, attempts cleanup, returns any error, and always
  drops the transport. Ordinary `Drop` does not perform hidden release writes

The device declares `INPUT_PROP_DIRECT`, virtual bus identity, `BTN_TOUCH`,
`ABS_MT_SLOT` with range `0..=slots-1`, `ABS_MT_TRACKING_ID` with range
`0..=65535`, and both MT and legacy X/Y ranges `0..=width-1` / `0..=height-1`.
Slots are bounded to 1–32 by the core. Coordinates represent pixels, so physical
axis resolution is left unspecified rather than falsely asserting millimeters.
Linux's `input_mt_init_slots` fixes this wire tracking range to 16 bits. The
adapter maps the core's monotonic `0..=i32::MAX` IDs to separate 16-bit wire IDs.
Wire IDs wrap while skipping all active IDs, including contacts released in the
same frame. A long-lived contact cannot collide with newer contacts after wrap.
The `-1` tracking value is the protocol's release sentinel, not an active ID.

## Error and recovery contract

There is **no atomic-write or downstream-delivery promise**. `evdev` writes the
body followed by a separate synchronization write. The kernel may accept a prefix
before a failure; even a failed final `SYN_REPORT` makes delivery uncertain.
Successful return means the transport reported body and sync completion under its
documented completion contract, not Android InputReader or app acknowledgment.

`AdapterError::Invalid` means no device write occurred and adapter state is
unchanged. The source allocator may already have advanced: stop and reconcile a
configuration or ordering mismatch rather than blindly sending later events.

Every transport error latches `DeviceState::Faulted`. `active_count()` returns
`None`, not zero or the last known count. All ordinary submissions and
`release_all()` are blocked. Failed releases never clear the adapter's contact
state or get reported as successful cleanup.

To recover the same device:

1. Stop accepting source input and discard every queued transition
2. Call the source allocator's `cancel_all()` and discard its local release batch
3. Explicitly call `resynchronize()`, which sends `BTN_TOUCH=0`, a `-1` tracking ID
   for **every** advertised slot, and one synchronization frame
4. Resume only if cleanup succeeds, using the **same allocator** so tracking IDs
   keep increasing. Each attempted down consumes its tracking ID even when the
   write fails; replaying that down is rejected

A failed resynchronization keeps the adapter faulted and its state unknown. The
caller can try another explicit resynchronization or destroy the device; there is
no retry loop or false remote-release acknowledgment. Destination dimension or slot-count changes require a
new device; a new source allocator also requires a new adapter. Destroying the
uinput handle removes the local virtual device, but is not proof of app-visible
contact cancellation. Power loss, process crash, consumer disconnect, Android
routing loss, and evdev reader overruns remain live system-level validation gates.

The pinned evdev writer handles ordinary short writes and interrupted writes, but
its internal `fd_write_all` treats a zero-byte write as termination. The actual
Linux uinput driver consumes complete aligned events or returns an error for a
nonempty valid write; this adapter relies on that kernel behavior and does not
independently inspect byte counts. Mock prefix/sync failures validate the adapter's
state machine, not the dependency's raw syscall implementation. A kernel/driver
that falsely returns zero progress is outside the tested transport guarantee.

The public `FrameTransport` seam enables isolated mock testing. A custom transport
must own an initially empty device exclusively, advertise the exact capabilities,
and return success only after writing all body events and one `SYN_REPORT` in
order. It may not silently replace the device or hide partial writes. Tests can
use this seam without device access.

## Validation commands

From the repository root:

```sh
cargo fmt --manifest-path core/input-linux/Cargo.toml --check
cargo clippy --manifest-path core/input-linux/Cargo.toml --locked --all-targets -- -D warnings
cargo test --manifest-path core/input-linux/Cargo.toml --locked
cargo test --manifest-path core/input-linux/Cargo.toml --locked --release
cargo build --manifest-path core/input-linux/Cargo.toml --locked --release
RUSTDOCFLAGS='-D warnings' cargo doc --manifest-path core/input-linux/Cargo.toml --locked --no-deps
```

The deterministic tests check exact type-B frames, primary-contact handoff,
multicontact focus-loss cleanup, strict bounds/identity/order validation,
side-effect-free rejection, 128 full 32-contact lifecycles, a 65,538-contact
wraparound sequence with a long-held contact, and every partial-write
boundary of a down frame including sync failure. They also exercise failed
releases, failed reset, explicit close, consumed tracking IDs, and recovery.
These are mocked transport tests, not kernel or Android delivery tests.

An additional ignored test calls the real constructor only after confirming that
`/dev/uinput` is absent. It refuses to run if any path exists there and does not
emit input, grant permissions, or attempt an alternative path:

```sh
# Only in an isolated environment where /dev/uinput is absent:
cargo test --manifest-path core/input-linux/Cargo.toml --locked \
  --test unavailable_device -- --ignored
```

On 2026-10-01 this environment had neither `/dev/uinput` nor `/dev/input`. The
real missing-device constructor test passed with `NotFound`. There were **no
live-device writes**. Hardware validation must still verify all capabilities,
one/two/maximum-contact gestures, exact post-kernel event semantics (unchanged
events may be filtered), focus-loss cancellation, device removal/recreation,
consumer disconnect/reconnect, Android `TOUCHSCREEN` classification, and
app-observed contacts and latency. Ordinary unit tests never open `/dev/uinput`.

## Protocol references

- [Linux type-B multi-touch protocol](https://www.kernel.org/doc/html/latest/input/multi-touch-protocol.html)
- [Linux event-code and BTN_TOUCH ordering semantics](https://www.kernel.org/doc/html/latest/input/event-codes.html)
- [evdev 0.13.2 VirtualDevice::emit](https://docs.rs/evdev/0.13.2/evdev/uinput/struct.VirtualDevice.html#method.emit)
- [evdev 0.13.2 VirtualDeviceBuilder](https://docs.rs/evdev/0.13.2/evdev/uinput/struct.VirtualDeviceBuilder.html)

- [Linux MT slot initialization and wire tracking range](https://github.com/torvalds/linux/blob/master/drivers/input/input-mt.c)
