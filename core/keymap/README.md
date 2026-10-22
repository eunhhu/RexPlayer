# rex-keymap

Window-scoped keyboard and mouse translation for RexPlayer. This crate accepts
host events already delivered to the player window; it never captures globally,
grabs a pointer, changes permissions, configures device routing, or implements
concealment. All profile parsing and controller tests run without a device.

## Profile format

`examples/default.json` is the built-in default. Schema version 1 supports:

- `hold`: one touch from a fresh source press until release
- `tap`: a down frame followed by an up frame, once per fresh press
- `joystick`: four distinct direction keys sharing one touch; initial center down,
  then a move to the directional point; diagonals have normalized length
- `pointer: true`: primary mouse down/drag/up maps the content rectangle to touch

Example:

```json
{
  "version": 1,
  "name": "Window controls",
  "pointer": true,
  "bindings": [
    {
      "kind": "joystick",
      "up": "w", "left": "a", "down": "s", "right": "d",
      "center": { "x": 0.2, "y": 0.75 },
      "radius": 0.15
    },
    {
      "kind": "hold",
      "trigger": { "kind": "key", "key": "space" },
      "point": { "x": 0.8, "y": 0.75 }
    },
    {
      "kind": "tap",
      "trigger": { "kind": "mouse", "button": "right" },
      "point": { "x": 0.8, "y": 0.5 }
    }
  ]
}
```

Coordinates are normalized within the displayed Android content before rotation.
A joystick's complete circle must fit in `[0,1]`. Its radius is measured in
normalized axes; an unrotated nonsquare content rectangle makes the physical
circle elliptical. Opposite directions cancel that axis. While any direction is
held, an all-opposed joystick remains touched at the center; all keys up releases.

Logical key names accept ASCII case-insensitively: `a`–`z`, `0`–`9`, `f1`–`f24`,
`space`, `enter`, `tab`, `backspace`, `delete`, `insert`, `home`, `end`, `pageup`,
`pagedown`, `up`, `down`, `left`, `right`, `shift`, `control`, and `alt`. Chords are
not supported. `escape` is always reserved for emergency release. Host adapters
must supply these logical names; physical scancode/layout conversion is upstream.
Mouse buttons are `left`, `right`, and `middle`; `left` cannot also be bound when
`pointer` is true. Each source occurs at most once, including joystick keys.

Profiles reject unknown/duplicate fields, unsupported versions, invalid geometry,
ambiguous sources, control characters in names, names over 80 bytes, and more than
32 total contact owners (including pointer). JSON is limited to 64 KiB before
parsing. `Keymap::from_reader` reads at most 64 KiB plus one byte before rejecting
an oversized file. `Keymap::from_json` validates existing strings; `to_json`
serializes the same schema. No profile is partially activated on error.

## Controller integration

```rust
use rex_input_core::{AndroidSize, Rotation};
use rex_keymap::{HostEvent, HostKey, Keymap, KeymapController, WindowViewport};

let mut source = KeymapController::new(
    Keymap::default(),
    WindowViewport::new(20.0, 30.0, 800.0, 600.0)?,
    AndroidSize::new(1080, 1920)?,
    Rotation::None,
)?;
source.handle(HostEvent::FocusGained)?;
let press = source.handle(HostEvent::KeyDown {
    key: HostKey::new("space")?, repeat: false,
})?;
let releases = source.handle(HostEvent::FocusLost)?;
// Deliver every transition in press, then releases, in its own adapter frame.
# Ok::<(), Box<dyn std::error::Error>>(())
```

The controller starts unfocused. Repeated downs, auto-repeat, and duplicate ups
produce no new contacts. Unknown keys consume no persistent state. Tap does not
retrigger until a source up and fresh down. Mouse presses must start inside the
actual content rectangle; drags clamp finite outside points to its edges. Mouse
release needs no coordinate. Invalid mouse movement leaves the contact available
for later release. Call `set_viewport` after resize and `set_geometry` after
rotation; both cancel all old contacts before changing coordinates. Supply the
real GPUI content bounds after letterboxing, using the same logical/physical pixel
units as mouse events. Final Android dimensions and rotation are explicit.

Focus loss and Escape/emergency release clear contacts and gate input. Pure
controller focus restoration requires `FocusGained`, and interrupted held/repeat
keys never recreate contacts. The host must discard stale queued events and feed
focus/resize/shutdown transitions; this library cannot observe an OS window.

## Linux session and GUI worker

Enable feature `linux-adapter` on Linux. `linux::LinuxInputSession` owns the source
controller and an existing `rex-input-linux` adapter. Its explicit `create` method
opens `/dev/uinput` using existing permissions. No default constructor opens a
device. `with_adapter` accepts an empty matching adapter for deterministic tests.
Each transition is submitted in a separate synchronization frame, preserving tap
and joystick lifecycles. Any adapter error clears and gates the source, discards
remaining transitions, and requires explicit resynchronization or close. Failed
batches are never replayed. Tracking IDs keep increasing after resynchronization.
Changing destination pixel dimensions requires a newly configured device.

GPUI must use `worker::InputWorker`, not synchronous device methods on the UI thread:

- `spawn(keymap, viewport, size, rotation)` starts explicit asynchronous creation
- `send(HostEvent)` and `set_viewport` use a bounded 64-command nonblocking queue
- After an explicit enable, send `FocusGained` only for the owning focused window
- `FocusLost`, fresh Escape, emergency release, and `shutdown()` use an out-of-band
  stop flag, discard queued input, and close the device on the worker thread
- Queue overflow also gates and closes the session; never retry the failed event
- `status()` is a nonblocking poll returning `Option<WorkerStatus>`
- Status states are `Starting`, `Ready`, `Stopping`, `Closed`, and `Failed`
- `wait_closed(timeout)` is for after the UI loop, never inside it; thread exit is
  distinct from successful cleanup, so inspect final status too

The conservative worker requires a fresh user enable after blur/escape/overflow
or errors. It never automatically recreates a device. Dropping its handle requests
asynchronous cleanup; retaining it lets the application observe cleanup status.
Device I/O already in progress cannot be forcibly interrupted by this wrapper.
A shutdown timeout or failure must not be reported as confirmed release delivery.

### Runtime routing limit

`Ready` proves that this process created a virtual touchscreen. Successful writes
prove only the adapter's transport completion contract. Neither proves Waydroid
can see the host event node, that a guest InputReader has adopted it, or that an
Android app received touch. The runtime must separately establish routing and
confirm touch down/move/up in an intended test app before labeling input delivered.
No namespace, device allowlist, permission, privileged service, or routing changes
are performed here. Missing `/dev/uinput` or permissions is surfaced as an explicit
startup failure. No real device was opened during the automated checks below.

## Checks

```sh
cargo fmt --manifest-path core/keymap/Cargo.toml --check
cargo clippy --manifest-path core/keymap/Cargo.toml --offline --all-targets --all-features -- -D warnings
cargo test --manifest-path core/keymap/Cargo.toml --offline --all-features
cargo test --manifest-path core/keymap/Cargo.toml --offline --all-features --release
```

Coverage includes schema limits and bounded readers; hold/tap/repeat behavior;
WASD diagonals/opposing/simultaneous keys; all rotations; pointer bounds, invalid
moves and release; resize/focus/emergency cleanup; 32 simultaneous owners; 20,000
mixed events against an independent contact consumer; real adapter encoding with
memory transport; partial-write and cleanup failure; startup cancellation; queue
overflow; and worker focus-loss shutdown. These are component/transport tests,
not host input capture or end-to-end Android routing validation.
