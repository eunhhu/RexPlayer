# rex-input-core

The first reusable input-state component for RexPlayer. This dependency-free Rust
library maps coordinates and manages a bounded set of contacts. It does not open
devices, inject input, capture keys, install hooks, communicate with Android, or
implement anti-cheat or integrity evasion. The existing `proof/input-rust` producer
is a separate runtime proof and is not wired to this library.

## API and guarantees

- `Viewport::new` and `AndroidSize::new` validate geometry before it can be used.
- `CoordinateMapping::map_viewport` normalizes a point in an edge-inclusive content
  rectangle; `map_normalized` accepts an already normalized point. Both apply the
  chosen rotation and map to pixel indices `0..=width-1`, `0..=height-1`.
- `BoundsPolicy::Reject` rejects finite out-of-range points. `Clamp` clamps them to
  the nearest edge. NaN and infinity always fail. Pixels use nearest-integer
  rounding with positive midpoint ties rounding up; a one-pixel axis maps to zero.
- `ContactAllocator::new` accepts 1 through 32 slots. `down` allocates the lowest
  free slot and emits a typed `Down`; duplicate downs are explicit errors.
- `move_to` keeps the slot and tracking ID, emitting `Move` only when a pixel changes.
  A rejected move leaves the previous valid contact active and releasable.
- `up` releases by caller identity, so invalid/missing coordinates cannot prevent
  cleanup. Duplicate or unknown ups are harmless and emit nothing.
- `cancel_all` and `focus_lost` return `Up` events for every active contact in slot
  order and leave the allocator empty. Repeated cleanup returns an empty batch.
- `set_mapping` releases every old contact before changing geometry, preserving the
  old coordinates in release events. It releases even if the mapping is identical.
- All returned errors leave contact state and the next tracking ID unchanged.
  Capacity exhaustion never evicts another contact. Tracking IDs increase from zero
  through `i32::MAX`, never wrap or repeat within an allocator, and fail closed at
  exhaustion. Existing contacts remain movable and releasable after exhaustion.

### Rotation convention

For normalized source `(u, v)`, clockwise transformations are:

| Rotation | Destination normalized point |
| --- | --- |
| `None` | `(u, v)` |
| `Clockwise90` | `(1-v, u)` |
| `Clockwise180` | `(1-u, 1-v)` |
| `Clockwise270` | `(v, 1-u)` |

Supply the final Android coordinate-space dimensions separately. These values are
not interpreted as Android orientation constants and dimensions are not swapped
implicitly. The caller chooses the forward/inverse rotation its display requires.

Viewport inputs must share a coordinate system with the configured rectangle.
Remove letterboxing and account for logical versus physical pixels upstream. The
right and bottom edges are included; they map to the final pixel, not one past it.
At extreme floating-point origins, effective extents use the representable edges.

## Integration boundary

The public Rust API is documented with a runnable example in `src/lib.rs`.

An eventual adapter owns transport, frame grouping, OS events, device capabilities,
error recovery, synchronization, and the actual Android application delivery test.
Deliver every returned transition in order. `Up` carries the ending contact's
positive tracking ID; translating that into any device-specific release sentinel is
the adapter's job. Tracking IDs are local to one allocator, not global across
instances. Caller contact IDs must identify the intended current contact; callers
must discard stale input from earlier gestures or source sessions.

On focus loss or interruption, stop accepting the interrupted source stream, call
cleanup, and deliver its entire batch before accepting new input. `focus_lost` only
clears state; it does not detect OS focus or block subsequent `down` calls. On a
transport failure, reset/reconcile the remote device before resuming. Dropping this
pure object cannot release an OS device, and ignored events or process crashes can
still leave remote contacts stuck. No end-to-end no-stuck-contact guarantee is
claimed without that adapter and its failure-path tests.

Operations require exclusive mutable access. There are no background threads,
timers, unsafe blocks, external dependencies, or performance claims. State uses a
small bounded vector; cleanup returns a newly allocated vector. This is not the
zero-allocation event ring buffer proposed in the architecture document.

## Local checks

From the repository root, with Rust/Cargo, rustfmt, and Clippy installed:

```sh
cargo fmt --manifest-path core/input/Cargo.toml --check
cargo clippy --manifest-path core/input/Cargo.toml --offline --all-targets -- -D warnings
cargo test --manifest-path core/input/Cargo.toml --offline
cargo test --manifest-path core/input/Cargo.toml --offline --release
cargo build --manifest-path core/input/Cargo.toml --offline --release
```

The tests cover every rotation and edge, nonfinite/out-of-range geometry, one-pixel
and large dimensions, capacity/slot reuse, tracking-ID exhaustion, repeated downs
and ups, coordinate-change suppression, remapping, cancellation, focus loss, 512
ten-contact interrupted lifecycles, and 20,000 deterministic mixed operations checked
against an in-memory event consumer. They do not test actual OS or Android delivery.
