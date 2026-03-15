//! C++ ↔ Rust FFI bridge via cxx
//!
//! This crate defines the boundary between the C++ VMM core and the Rust
//! virtio device backends. The C++ side dispatches MMIO/IO exits to Rust,
//! and Rust can trigger interrupts back into the VM.

#[cxx::bridge(namespace = "rex::ffi")]
mod ffi {
    /// MMIO access from C++ vCPU exit → Rust device
    struct MmioRequest {
        address: u64,
        size: u8,
        is_write: bool,
        data: u64,
    }

    /// I/O port access from C++ vCPU exit → Rust device
    struct IoRequest {
        port: u16,
        size: u8,
        is_write: bool,
        data: u32,
    }

    /// Result of processing a device request
    struct DeviceResponse {
        data: u64,
        handled: bool,
    }

    /// Interrupt request from Rust device → C++ VMM
    struct IrqRequest {
        irq: u32,
        level: bool,
    }

    extern "Rust" {
        /// Initialize the Rust middleware (called once at startup)
        fn middleware_init();

        /// Handle an MMIO access from the vCPU
        fn handle_mmio(req: &mut MmioRequest) -> DeviceResponse;

        /// Handle an I/O port access from the vCPU
        fn handle_io(req: &mut IoRequest) -> DeviceResponse;

        /// Tick the middleware (called periodically for timers, etc.)
        fn middleware_tick();
    }

    unsafe extern "C++" {
        include!("rex/ffi/callbacks.h");

        /// Inject an interrupt into the VM (Rust → C++)
        fn inject_irq(req: IrqRequest);

        /// Read guest memory (Rust → C++)
        unsafe fn read_guest_memory(gpa: u64, buf: *mut u8, len: usize) -> bool;

        /// Write guest memory (Rust → C++)
        unsafe fn write_guest_memory(gpa: u64, buf: *const u8, len: usize) -> bool;
    }
}

use ffi::{DeviceResponse, IoRequest, MmioRequest};

/// Global device registry (will be populated with virtio devices)
fn middleware_init() {
    tracing::info!("RexPlayer Rust middleware initialized");
}

fn handle_mmio(req: &mut MmioRequest) -> DeviceResponse {
    // TODO: dispatch to registered virtio-mmio devices
    let _ = req;
    DeviceResponse {
        data: 0,
        handled: false,
    }
}

fn handle_io(req: &mut IoRequest) -> DeviceResponse {
    // TODO: dispatch to registered I/O devices
    let _ = req;
    DeviceResponse {
        data: 0,
        handled: false,
    }
}

fn middleware_tick() {
    // TODO: process async completions, timer events, etc.
}
