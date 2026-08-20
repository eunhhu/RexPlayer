use anyhow::{bail, Context, Result};
use evdev::{
    uinput::VirtualDevice, AbsInfo, AbsoluteAxisCode, AttributeSet, Device, EventType, InputEvent,
    KeyCode, UinputAbsSetup,
};
use std::{
    io::ErrorKind,
    thread,
    time::{Duration, Instant},
};

fn abs(code: AbsoluteAxisCode, max: i32) -> UinputAbsSetup {
    UinputAbsSetup::new(code, AbsInfo::new(0, 0, max, 0, 0, 1))
}

fn event(event_type: EventType, code: u16, value: i32) -> InputEvent {
    InputEvent::new(event_type.0, code, value)
}

fn main() -> Result<()> {
    let mut keys = AttributeSet::<KeyCode>::new();
    keys.insert(KeyCode::BTN_TOUCH);

    let slot = abs(AbsoluteAxisCode::ABS_MT_SLOT, 9);
    let tracking = abs(AbsoluteAxisCode::ABS_MT_TRACKING_ID, 65_535);
    let mt_x = abs(AbsoluteAxisCode::ABS_MT_POSITION_X, 1_079);
    let mt_y = abs(AbsoluteAxisCode::ABS_MT_POSITION_Y, 1_919);
    let x = abs(AbsoluteAxisCode::ABS_X, 1_079);
    let y = abs(AbsoluteAxisCode::ABS_Y, 1_919);

    let mut virtual_device = VirtualDevice::builder()
        .context("open /dev/uinput")?
        .name("RexPlayer Rust Multi-Touch Proof")
        .with_keys(&keys)?
        .with_absolute_axis(&slot)?
        .with_absolute_axis(&tracking)?
        .with_absolute_axis(&mt_x)?
        .with_absolute_axis(&mt_y)?
        .with_absolute_axis(&x)?
        .with_absolute_axis(&y)?
        .build()?;

    thread::sleep(Duration::from_millis(300));
    let node = virtual_device
        .enumerate_dev_nodes_blocking()?
        .next()
        .context("virtual device had no /dev/input/event node")??;
    println!("DEVICE node={}", node.display());

    let mut reader = Device::open(&node).with_context(|| format!("open {}", node.display()))?;
    reader.set_nonblocking(true)?;

    virtual_device.emit(&[
        event(EventType::KEY, KeyCode::BTN_TOUCH.0, 1),
        event(EventType::ABSOLUTE, AbsoluteAxisCode::ABS_MT_SLOT.0, 0),
        event(
            EventType::ABSOLUTE,
            AbsoluteAxisCode::ABS_MT_TRACKING_ID.0,
            42,
        ),
        event(
            EventType::ABSOLUTE,
            AbsoluteAxisCode::ABS_MT_POSITION_X.0,
            100,
        ),
        event(
            EventType::ABSOLUTE,
            AbsoluteAxisCode::ABS_MT_POSITION_Y.0,
            200,
        ),
        event(EventType::ABSOLUTE, AbsoluteAxisCode::ABS_X.0, 100),
        event(EventType::ABSOLUTE, AbsoluteAxisCode::ABS_Y.0, 200),
    ])?;
    virtual_device.emit(&[
        event(
            EventType::ABSOLUTE,
            AbsoluteAxisCode::ABS_MT_POSITION_X.0,
            400,
        ),
        event(
            EventType::ABSOLUTE,
            AbsoluteAxisCode::ABS_MT_POSITION_Y.0,
            500,
        ),
        event(EventType::ABSOLUTE, AbsoluteAxisCode::ABS_X.0, 400),
        event(EventType::ABSOLUTE, AbsoluteAxisCode::ABS_Y.0, 500),
    ])?;
    virtual_device.emit(&[
        event(
            EventType::ABSOLUTE,
            AbsoluteAxisCode::ABS_MT_TRACKING_ID.0,
            -1,
        ),
        event(EventType::KEY, KeyCode::BTN_TOUCH.0, 0),
    ])?;

    // ABS_MT_SLOT=0 is unchanged from the initial slot and is filtered by evdev.
    let expected = [
        (EventType::KEY.0, KeyCode::BTN_TOUCH.0, 1),
        (
            EventType::ABSOLUTE.0,
            AbsoluteAxisCode::ABS_MT_TRACKING_ID.0,
            42,
        ),
        (
            EventType::ABSOLUTE.0,
            AbsoluteAxisCode::ABS_MT_POSITION_X.0,
            100,
        ),
        (
            EventType::ABSOLUTE.0,
            AbsoluteAxisCode::ABS_MT_POSITION_Y.0,
            200,
        ),
        (EventType::ABSOLUTE.0, AbsoluteAxisCode::ABS_X.0, 100),
        (EventType::ABSOLUTE.0, AbsoluteAxisCode::ABS_Y.0, 200),
        (EventType::SYNCHRONIZATION.0, 0, 0),
        (
            EventType::ABSOLUTE.0,
            AbsoluteAxisCode::ABS_MT_POSITION_X.0,
            400,
        ),
        (
            EventType::ABSOLUTE.0,
            AbsoluteAxisCode::ABS_MT_POSITION_Y.0,
            500,
        ),
        (EventType::ABSOLUTE.0, AbsoluteAxisCode::ABS_X.0, 400),
        (EventType::ABSOLUTE.0, AbsoluteAxisCode::ABS_Y.0, 500),
        (EventType::SYNCHRONIZATION.0, 0, 0),
        (
            EventType::ABSOLUTE.0,
            AbsoluteAxisCode::ABS_MT_TRACKING_ID.0,
            -1,
        ),
        (EventType::KEY.0, KeyCode::BTN_TOUCH.0, 0),
        (EventType::SYNCHRONIZATION.0, 0, 0),
    ];
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut observed = Vec::with_capacity(expected.len());

    while Instant::now() < deadline && observed.len() < expected.len() {
        match reader.fetch_events() {
            Ok(events) => {
                for ev in events {
                    println!(
                        "EVENT type={} code={} value={}",
                        ev.event_type().0,
                        ev.code(),
                        ev.value()
                    );
                    observed.push((ev.event_type().0, ev.code(), ev.value()));
                }
            }
            Err(err) if err.kind() == ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(10))
            }
            Err(err) => return Err(err.into()),
        }
    }

    let pass = observed.as_slice() == expected;
    let syn = observed
        .iter()
        .filter(|(event_type, code, _)| *event_type == EventType::SYNCHRONIZATION.0 && *code == 0)
        .count();
    println!(
        "SUMMARY events={} expected={} syn_reports={syn} sequence={}",
        observed.len(),
        expected.len(),
        pass as u8
    );
    println!("RESULT {}", if pass { "PASS" } else { "FAIL" });
    if !pass {
        bail!("multitouch event verification failed");
    }
    Ok(())
}
