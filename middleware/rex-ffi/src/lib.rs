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

    /// GPU resource creation request (Rust → C++)
    struct GpuResourceCreate {
        resource_id: u32,
        format: u32,
        width: u32,
        height: u32,
    }

    /// GPU transfer request (Rust → C++)
    struct GpuTransfer {
        resource_id: u32,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        offset: u64,
    }

    /// GPU scanout configuration (Rust → C++)
    struct GpuScanout {
        scanout_id: u32,
        resource_id: u32,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
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

        /// Get the number of registered virtio-mmio devices
        fn device_count() -> u32;
    }

    unsafe extern "C++" {
        include!("rex/ffi/callbacks.h");

        /// Inject an interrupt into the VM (Rust → C++)
        fn inject_irq(req: IrqRequest);

        /// Read guest memory (Rust → C++)
        unsafe fn read_guest_memory(gpa: u64, buf: *mut u8, len: usize) -> bool;

        /// Write guest memory (Rust → C++)
        unsafe fn write_guest_memory(gpa: u64, buf: *const u8, len: usize) -> bool;

        /// GPU: create a 2D resource on the host renderer
        fn gpu_resource_create_2d(req: &GpuResourceCreate) -> bool;

        /// GPU: destroy a resource
        fn gpu_resource_unref(resource_id: u32);

        /// GPU: set scanout
        fn gpu_set_scanout(req: &GpuScanout) -> bool;

        /// GPU: flush resource region to display
        fn gpu_resource_flush(req: &GpuTransfer) -> bool;
    }
}

use ffi::{DeviceResponse, IoRequest, MmioRequest};
use std::sync::atomic::{AtomicU32, Ordering};

/// Number of registered devices
static DEVICE_COUNT: AtomicU32 = AtomicU32::new(0);

fn middleware_init() {
    tracing::info!("RexPlayer Rust middleware initialized");
    // In a full implementation, this would:
    // 1. Parse config to determine which devices to create
    // 2. Create virtio-mmio device instances
    // 3. Register them at their MMIO base addresses
    DEVICE_COUNT.store(0, Ordering::SeqCst);
}

fn handle_mmio(req: &mut MmioRequest) -> DeviceResponse {
    // TODO: dispatch to registered virtio-mmio devices based on address
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
    // Process async completions, poll network, etc.
}

fn device_count() -> u32 {
    DEVICE_COUNT.load(Ordering::SeqCst)
}
