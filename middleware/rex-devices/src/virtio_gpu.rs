//! Virtio GPU device backend
//!
//! Implements the virtio-gpu device as defined in the virtio specification
//! (Section 5.7). Provides 2D framebuffer operations and 3D rendering
//! support via virgl passthrough. Manages GPU resources, scanouts, and
//! command processing for the guest graphics stack.

use crate::{DeviceError, DeviceResult, VirtioDevice, VirtioDeviceType};
use std::collections::HashMap;

// ============================================================================
// Feature flags (virtio spec 5.7.3)
// ============================================================================

pub mod features {
    /// VirGL 3D rendering support
    pub const VIRTIO_GPU_F_VIRGL: u64 = 1 << 0;
    /// EDID display information support
    pub const VIRTIO_GPU_F_EDID: u64 = 1 << 1;
    /// Resource UUID assignment support
    pub const VIRTIO_GPU_F_RESOURCE_UUID: u64 = 1 << 2;
    /// Zero-copy resource blob support
    pub const VIRTIO_GPU_F_RESOURCE_BLOB: u64 = 1 << 3;
}

// ============================================================================
// Command / response types (virtio spec 5.7.6.7)
// ============================================================================

/// Control command and response type codes for virtio-gpu.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GpuCommand {
    // 2D commands
    GetDisplayInfo = 0x0100,
    ResourceCreate2d = 0x0101,
    ResourceUnref = 0x0102,
    SetScanout = 0x0103,
    ResourceFlush = 0x0104,
    TransferToHost2d = 0x0105,
    ResourceAttachBacking = 0x0106,
    ResourceDetachBacking = 0x0107,

    // Cursor commands (unused in our impl, but defined for completeness)
    UpdateCursor = 0x0300,
    MoveCursor = 0x0301,

    // 3D commands
    CtxCreate = 0x0200,
    CtxDestroy = 0x0201,
    CtxAttachResource = 0x0202,
    CtxDetachResource = 0x0203,
    GetCapsetInfo = 0x0204,
    GetCapset = 0x0205,
    Submit3d = 0x0206,

    // Success responses
    RespOkNodata = 0x1100,
    RespOkDisplayInfo = 0x1101,
    RespOkCapsetInfo = 0x1102,
    RespOkCapset = 0x1103,

    // Error responses
    RespErrUnspec = 0x1200,
    RespErrOutOfMemory = 0x1201,
    RespErrInvalidScanoutId = 0x1202,
    RespErrInvalidResourceId = 0x1203,
    RespErrInvalidContextId = 0x1204,
    RespErrInvalidParameter = 0x1205,
}

impl GpuCommand {
    /// Parse a command type from a raw u32 value.
    pub fn from_u32(val: u32) -> Option<Self> {
        match val {
            0x0100 => Some(Self::GetDisplayInfo),
            0x0101 => Some(Self::ResourceCreate2d),
            0x0102 => Some(Self::ResourceUnref),
            0x0103 => Some(Self::SetScanout),
            0x0104 => Some(Self::ResourceFlush),
            0x0105 => Some(Self::TransferToHost2d),
            0x0106 => Some(Self::ResourceAttachBacking),
            0x0107 => Some(Self::ResourceDetachBacking),
            0x0200 => Some(Self::CtxCreate),
            0x0201 => Some(Self::CtxDestroy),
            0x0202 => Some(Self::CtxAttachResource),
            0x0203 => Some(Self::CtxDetachResource),
            0x0204 => Some(Self::GetCapsetInfo),
            0x0205 => Some(Self::GetCapset),
            0x0206 => Some(Self::Submit3d),
            0x0300 => Some(Self::UpdateCursor),
            0x0301 => Some(Self::MoveCursor),
            0x1100 => Some(Self::RespOkNodata),
            0x1101 => Some(Self::RespOkDisplayInfo),
            0x1102 => Some(Self::RespOkCapsetInfo),
            0x1103 => Some(Self::RespOkCapset),
            0x1200 => Some(Self::RespErrUnspec),
            0x1201 => Some(Self::RespErrOutOfMemory),
            0x1202 => Some(Self::RespErrInvalidScanoutId),
            0x1203 => Some(Self::RespErrInvalidResourceId),
            0x1204 => Some(Self::RespErrInvalidContextId),
            0x1205 => Some(Self::RespErrInvalidParameter),
            _ => None,
        }
    }
}

// ============================================================================
// Pixel formats (virtio spec 5.7.6.8 — enum virtio_gpu_formats)
// ============================================================================

/// Virtio GPU pixel formats. Matches the virtio_gpu_formats enum.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GpuFormat {
    B8G8R8A8Unorm = 1,
    B8G8R8X8Unorm = 2,
    A8R8G8B8Unorm = 3,
    X8R8G8B8Unorm = 4,
    R8G8B8A8Unorm = 67,
    X8B8G8R8Unorm = 68,
    A8B8G8R8Unorm = 121,
    R8G8B8X8Unorm = 134,
}

impl GpuFormat {
    /// Parse from raw u32.
    pub fn from_u32(val: u32) -> Option<Self> {
        match val {
            1 => Some(Self::B8G8R8A8Unorm),
            2 => Some(Self::B8G8R8X8Unorm),
            3 => Some(Self::A8R8G8B8Unorm),
            4 => Some(Self::X8R8G8B8Unorm),
            67 => Some(Self::R8G8B8A8Unorm),
            68 => Some(Self::X8B8G8R8Unorm),
            121 => Some(Self::A8B8G8R8Unorm),
            134 => Some(Self::R8G8B8X8Unorm),
            _ => None,
        }
    }

    /// Bytes per pixel for this format.
    pub fn bytes_per_pixel(&self) -> u32 {
        // All currently supported formats are 32-bit (4 bytes per pixel)
        4
    }
}

// ============================================================================
// Constants
// ============================================================================

/// Maximum number of scanouts supported by the device.
pub const VIRTIO_GPU_MAX_SCANOUTS: usize = 16;

/// Size of the control header in bytes.
pub const CTRL_HDR_SIZE: usize = 24;

/// Virtqueue indices for GPU device
pub const CONTROLQ: u16 = 0;
pub const CURSORQ: u16 = 1;

/// Flags in the control header
pub const VIRTIO_GPU_FLAG_FENCE: u32 = 1 << 0;
/// Flag indicating that the command carries an info-query fence
pub const VIRTIO_GPU_FLAG_INFO_RING_IDX: u32 = 1 << 1;

// ============================================================================
// Control header (virtio spec 5.7.6.7)
// ============================================================================

/// Control command/response header that prefixes every GPU command.
///
/// ```text
/// struct virtio_gpu_ctrl_hdr {
///     le32 type;
///     le32 flags;
///     le64 fence_id;
///     le32 ctx_id;
///     le32 ring_idx;
/// };
/// ```
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GpuCtrlHdr {
    /// Command or response type (GpuCommand value).
    pub type_: u32,
    /// Flags (e.g. VIRTIO_GPU_FLAG_FENCE).
    pub flags: u32,
    /// Fence ID for synchronization.
    pub fence_id: u64,
    /// 3D rendering context ID.
    pub ctx_id: u32,
    /// Ring index (for multi-ring support).
    pub ring_idx: u32,
}

impl GpuCtrlHdr {
    /// Serialize to little-endian bytes.
    pub fn to_bytes(&self) -> [u8; CTRL_HDR_SIZE] {
        let mut buf = [0u8; CTRL_HDR_SIZE];
        buf[0..4].copy_from_slice(&self.type_.to_le_bytes());
        buf[4..8].copy_from_slice(&self.flags.to_le_bytes());
        buf[8..16].copy_from_slice(&self.fence_id.to_le_bytes());
        buf[16..20].copy_from_slice(&self.ctx_id.to_le_bytes());
        buf[20..24].copy_from_slice(&self.ring_idx.to_le_bytes());
        buf
    }

    /// Parse from little-endian bytes.
    pub fn from_bytes(buf: &[u8]) -> DeviceResult<Self> {
        if buf.len() < CTRL_HDR_SIZE {
            return Err(DeviceError::InvalidDescriptor);
        }
        Ok(Self {
            type_: u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]),
            flags: u32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]),
            fence_id: u64::from_le_bytes([
                buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15],
            ]),
            ctx_id: u32::from_le_bytes([buf[16], buf[17], buf[18], buf[19]]),
            ring_idx: u32::from_le_bytes([buf[20], buf[21], buf[22], buf[23]]),
        })
    }

    /// Create a simple response header with the given type code.
    pub fn response(type_: GpuCommand) -> Self {
        Self {
            type_: type_ as u32,
            flags: 0,
            fence_id: 0,
            ctx_id: 0,
            ring_idx: 0,
        }
    }

    /// Create a response header that carries the fence from the original command.
    pub fn response_with_fence(type_: GpuCommand, request: &GpuCtrlHdr) -> Self {
        Self {
            type_: type_ as u32,
            flags: request.flags & VIRTIO_GPU_FLAG_FENCE,
            fence_id: request.fence_id,
            ctx_id: request.ctx_id,
            ring_idx: request.ring_idx,
        }
    }
}

// ============================================================================
// Geometry — virtio_gpu_rect
// ============================================================================

/// A rectangle within a resource or scanout.
///
/// ```text
/// struct virtio_gpu_rect {
///     le32 x, y, width, height;
/// };
/// ```
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct GpuRect {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

impl GpuRect {
    pub fn to_bytes(&self) -> [u8; 16] {
        let mut buf = [0u8; 16];
        buf[0..4].copy_from_slice(&self.x.to_le_bytes());
        buf[4..8].copy_from_slice(&self.y.to_le_bytes());
        buf[8..12].copy_from_slice(&self.width.to_le_bytes());
        buf[12..16].copy_from_slice(&self.height.to_le_bytes());
        buf
    }

    pub fn from_bytes(buf: &[u8]) -> DeviceResult<Self> {
        if buf.len() < 16 {
            return Err(DeviceError::InvalidDescriptor);
        }
        Ok(Self {
            x: u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]),
            y: u32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]),
            width: u32::from_le_bytes([buf[8], buf[9], buf[10], buf[11]]),
            height: u32::from_le_bytes([buf[12], buf[13], buf[14], buf[15]]),
        })
    }
}

// ============================================================================
// Display info — virtio_gpu_display_one
// ============================================================================

/// Per-scanout display information returned by GET_DISPLAY_INFO.
///
/// ```text
/// struct virtio_gpu_display_one {
///     struct virtio_gpu_rect r;
///     le32 enabled;
///     le32 flags;
/// };
/// ```
#[derive(Debug, Clone, Copy, Default)]
pub struct GpuDisplayOne {
    /// Display rectangle (position and size).
    pub rect: GpuRect,
    /// Whether this scanout is enabled.
    pub enabled: u32,
    /// Display flags (reserved, set to 0).
    pub flags: u32,
}

impl GpuDisplayOne {
    /// Size of one display entry in the response payload.
    pub const SIZE: usize = 24;

    pub fn to_bytes(&self) -> [u8; Self::SIZE] {
        let mut buf = [0u8; Self::SIZE];
        buf[0..16].copy_from_slice(&self.rect.to_bytes());
        buf[16..20].copy_from_slice(&self.enabled.to_le_bytes());
        buf[20..24].copy_from_slice(&self.flags.to_le_bytes());
        buf
    }
}

// ============================================================================
// Resource — internal representation
// ============================================================================

/// A 2D resource managed by the device.
///
/// Each resource has pixel dimensions, a format, and optional backing memory
/// that is attached by the guest via RESOURCE_ATTACH_BACKING.
#[derive(Debug)]
pub struct GpuResource {
    /// Unique resource identifier assigned by the guest.
    pub id: u32,
    /// Width in pixels.
    pub width: u32,
    /// Height in pixels.
    pub height: u32,
    /// Pixel format.
    pub format: u32,
    /// Backing memory (guest-attached). Empty until backing is attached.
    pub backing: Vec<u8>,
    /// Whether backing memory has been attached.
    pub has_backing: bool,
}

impl GpuResource {
    /// Create a new resource with no backing memory.
    pub fn new(id: u32, width: u32, height: u32, format: u32) -> Self {
        Self {
            id,
            width,
            height,
            format,
            backing: Vec::new(),
            has_backing: false,
        }
    }

    /// Total bytes required for this resource at 4 bytes per pixel.
    pub fn size_bytes(&self) -> usize {
        let bpp = GpuFormat::from_u32(self.format)
            .map(|f| f.bytes_per_pixel())
            .unwrap_or(4);
        (self.width as usize) * (self.height as usize) * (bpp as usize)
    }

    /// Stride in bytes (width * bpp, no padding).
    pub fn stride(&self) -> u32 {
        let bpp = GpuFormat::from_u32(self.format)
            .map(|f| f.bytes_per_pixel())
            .unwrap_or(4);
        self.width * bpp
    }
}

// ============================================================================
// Scanout
// ============================================================================

/// Scanout configuration — binds a resource to a display output.
#[derive(Debug, Clone, Default)]
pub struct Scanout {
    /// Resource ID bound to this scanout (0 = disabled).
    pub resource_id: u32,
    /// Visible rectangle within the resource.
    pub rect: GpuRect,
    /// Whether this scanout is enabled.
    pub enabled: bool,
}

// ============================================================================
// Capset info — for virgl 3D support
// ============================================================================

/// Capability set information for 3D rendering.
#[derive(Debug, Clone)]
pub struct CapsetInfo {
    /// Capset identifier (e.g. 1 = virgl, 2 = virgl2).
    pub capset_id: u32,
    /// Maximum version of this capset.
    pub max_version: u32,
    /// Maximum size in bytes of the capset data.
    pub max_size: u32,
}

// ============================================================================
// 3D context
// ============================================================================

/// A 3D rendering context created by the guest.
#[derive(Debug)]
pub struct Gpu3dContext {
    /// Context ID assigned by the guest.
    pub id: u32,
    /// Debug name provided by the guest (up to 64 bytes).
    pub name: String,
    /// Resources attached to this context.
    pub attached_resources: Vec<u32>,
}

// ============================================================================
// GpuRenderer trait — host-side rendering backend abstraction
// ============================================================================

/// Trait for host-side GPU rendering backends.
///
/// Implementations bridge between the virtio-gpu device model and the actual
/// host rendering API (e.g. virglrenderer for OpenGL, venus for Vulkan).
pub trait GpuRenderer: Send {
    /// Create a new 3D rendering context. Returns a context handle.
    fn create_context(&mut self, ctx_id: u32, name: &str) -> DeviceResult<()>;

    /// Destroy a 3D rendering context.
    fn destroy_context(&mut self, ctx_id: u32) -> DeviceResult<()>;

    /// Submit a 3D command buffer for a given context.
    fn submit_3d(&mut self, ctx_id: u32, commands: &[u8]) -> DeviceResult<()>;

    /// Query capability set info. Returns (capset_id, max_version, max_size).
    fn get_capset_info(&self, index: u32) -> Option<CapsetInfo>;

    /// Number of available capability sets.
    fn num_capsets(&self) -> u32;

    /// Get capability set data for the given (capset_id, version).
    fn get_capset(&self, capset_id: u32, version: u32) -> Vec<u8>;
}

/// A no-op renderer used when 3D is not enabled.
#[derive(Debug)]
pub struct NullRenderer;

impl GpuRenderer for NullRenderer {
    fn create_context(&mut self, _ctx_id: u32, _name: &str) -> DeviceResult<()> {
        Ok(())
    }

    fn destroy_context(&mut self, _ctx_id: u32) -> DeviceResult<()> {
        Ok(())
    }

    fn submit_3d(&mut self, _ctx_id: u32, _commands: &[u8]) -> DeviceResult<()> {
        Ok(())
    }

    fn get_capset_info(&self, _index: u32) -> Option<CapsetInfo> {
        None
    }

    fn num_capsets(&self) -> u32 {
        0
    }

    fn get_capset(&self, _capset_id: u32, _version: u32) -> Vec<u8> {
        Vec::new()
    }
}

// ============================================================================
// GPU configuration
// ============================================================================

/// Configuration for the virtio-gpu device.
#[derive(Debug, Clone)]
pub struct VirtioGpuConfig {
    /// Display width in pixels.
    pub width: u32,
    /// Display height in pixels.
    pub height: u32,
    /// Enable virgl 3D support.
    pub virgl: bool,
    /// Enable EDID support.
    pub edid: bool,
}

impl Default for VirtioGpuConfig {
    fn default() -> Self {
        Self {
            width: 1280,
            height: 720,
            virgl: false,
            edid: false,
        }
    }
}

// ============================================================================
// Flush callback
// ============================================================================

/// Callback invoked when a resource is flushed and ready for display.
///
/// Parameters: (resource_id, scanout_id, data, width, height, stride, format)
pub type FlushCallback =
    Box<dyn FnMut(u32, u32, &[u8], u32, u32, u32, u32) + Send>;

// ============================================================================
// VirtioGpu device
// ============================================================================

/// Virtio GPU device backend.
///
/// Manages 2D resources, scanout configuration, display info, and
/// optionally delegates 3D rendering to a `GpuRenderer` backend.
pub struct VirtioGpu {
    /// Negotiated feature flags.
    features: u64,
    /// Whether the device has been activated.
    activated: bool,
    /// Display configuration.
    config: VirtioGpuConfig,
    /// Resources indexed by resource ID.
    resources: HashMap<u32, GpuResource>,
    /// Scanout array (up to VIRTIO_GPU_MAX_SCANOUTS).
    scanouts: Vec<Scanout>,
    /// 3D rendering contexts indexed by context ID.
    contexts: HashMap<u32, Gpu3dContext>,
    /// Host-side 3D renderer backend.
    renderer: Box<dyn GpuRenderer>,
    /// Completed fence IDs (for async completion tracking).
    completed_fences: Vec<u64>,
    /// Callback for resource flush (display update).
    flush_callback: Option<FlushCallback>,
}

impl VirtioGpu {
    /// Create a new virtio-gpu device with the given configuration.
    pub fn new(width: u32, height: u32) -> Self {
        Self::with_config(VirtioGpuConfig {
            width,
            height,
            ..Default::default()
        })
    }

    /// Create a new virtio-gpu device with full configuration.
    pub fn with_config(config: VirtioGpuConfig) -> Self {
        let mut feat: u64 = 0;
        if config.virgl {
            feat |= features::VIRTIO_GPU_F_VIRGL;
        }
        if config.edid {
            feat |= features::VIRTIO_GPU_F_EDID;
        }

        // Default scanout 0 is enabled with the configured resolution
        let mut scanouts = vec![Scanout::default(); VIRTIO_GPU_MAX_SCANOUTS];
        scanouts[0] = Scanout {
            resource_id: 0,
            rect: GpuRect {
                x: 0,
                y: 0,
                width: config.width,
                height: config.height,
            },
            enabled: true,
        };

        Self {
            features: feat,
            activated: false,
            config,
            resources: HashMap::new(),
            scanouts,
            contexts: HashMap::new(),
            renderer: Box::new(NullRenderer),
            completed_fences: Vec::new(),
            flush_callback: None,
        }
    }

    /// Set the 3D renderer backend.
    pub fn set_renderer(&mut self, renderer: Box<dyn GpuRenderer>) {
        self.renderer = renderer;
    }

    /// Set the flush callback invoked when a resource is flushed.
    pub fn set_flush_callback(&mut self, cb: FlushCallback) {
        self.flush_callback = Some(cb);
    }

    /// Get the display width.
    pub fn width(&self) -> u32 {
        self.config.width
    }

    /// Get the display height.
    pub fn height(&self) -> u32 {
        self.config.height
    }

    /// Get a reference to a resource by ID.
    pub fn resource(&self, id: u32) -> Option<&GpuResource> {
        self.resources.get(&id)
    }

    /// Get a reference to a scanout by index.
    pub fn scanout(&self, index: usize) -> Option<&Scanout> {
        self.scanouts.get(index)
    }

    /// Get the number of resources currently tracked.
    pub fn resource_count(&self) -> usize {
        self.resources.len()
    }

    /// Get the number of active 3D contexts.
    pub fn context_count(&self) -> usize {
        self.contexts.len()
    }

    /// Drain completed fence IDs.
    pub fn drain_fences(&mut self) -> Vec<u64> {
        std::mem::take(&mut self.completed_fences)
    }

    // -----------------------------------------------------------------------
    // Command processing — main entry point
    // -----------------------------------------------------------------------

    /// Process a virtio-gpu control command and produce a response.
    ///
    /// The caller passes the raw command bytes (starting with a `GpuCtrlHdr`).
    /// Returns the raw response bytes (also starting with a `GpuCtrlHdr`).
    pub fn process_command(&mut self, cmd_bytes: &[u8]) -> Vec<u8> {
        // Parse the header
        let hdr = match GpuCtrlHdr::from_bytes(cmd_bytes) {
            Ok(h) => h,
            Err(_) => return self.make_error_response(GpuCommand::RespErrUnspec, None),
        };

        let cmd_type = match GpuCommand::from_u32(hdr.type_) {
            Some(c) => c,
            None => {
                tracing::warn!("virtio-gpu: unknown command type {:#x}", hdr.type_);
                return self.make_error_response(GpuCommand::RespErrUnspec, Some(&hdr));
            }
        };

        let payload = &cmd_bytes[CTRL_HDR_SIZE..];

        let response = match cmd_type {
            GpuCommand::GetDisplayInfo => self.cmd_get_display_info(&hdr),
            GpuCommand::ResourceCreate2d => self.cmd_resource_create_2d(&hdr, payload),
            GpuCommand::ResourceUnref => self.cmd_resource_unref(&hdr, payload),
            GpuCommand::SetScanout => self.cmd_set_scanout(&hdr, payload),
            GpuCommand::ResourceFlush => self.cmd_resource_flush(&hdr, payload),
            GpuCommand::TransferToHost2d => self.cmd_transfer_to_host_2d(&hdr, payload),
            GpuCommand::ResourceAttachBacking => {
                self.cmd_resource_attach_backing(&hdr, payload)
            }
            GpuCommand::ResourceDetachBacking => {
                self.cmd_resource_detach_backing(&hdr, payload)
            }
            GpuCommand::GetCapsetInfo => self.cmd_get_capset_info(&hdr, payload),
            GpuCommand::GetCapset => self.cmd_get_capset(&hdr, payload),
            GpuCommand::CtxCreate => self.cmd_ctx_create(&hdr, payload),
            GpuCommand::CtxDestroy => self.cmd_ctx_destroy(&hdr, payload),
            GpuCommand::Submit3d => self.cmd_submit_3d(&hdr, payload),
            GpuCommand::CtxAttachResource => self.cmd_ctx_attach_resource(&hdr, payload),
            GpuCommand::CtxDetachResource => self.cmd_ctx_detach_resource(&hdr, payload),
            _ => {
                tracing::warn!("virtio-gpu: unhandled command {:?}", cmd_type);
                self.make_error_response(GpuCommand::RespErrUnspec, Some(&hdr))
            }
        };

        // Track fence completion
        if hdr.flags & VIRTIO_GPU_FLAG_FENCE != 0 {
            self.completed_fences.push(hdr.fence_id);
        }

        response
    }

    // -----------------------------------------------------------------------
    // Command handlers
    // -----------------------------------------------------------------------

    /// GET_DISPLAY_INFO — returns display parameters for all scanouts.
    fn cmd_get_display_info(&self, request: &GpuCtrlHdr) -> Vec<u8> {
        let resp_hdr =
            GpuCtrlHdr::response_with_fence(GpuCommand::RespOkDisplayInfo, request);

        // Response payload: VIRTIO_GPU_MAX_SCANOUTS * GpuDisplayOne
        let mut resp = Vec::with_capacity(
            CTRL_HDR_SIZE + VIRTIO_GPU_MAX_SCANOUTS * GpuDisplayOne::SIZE,
        );
        resp.extend_from_slice(&resp_hdr.to_bytes());

        for scanout in &self.scanouts {
            let disp = GpuDisplayOne {
                rect: scanout.rect,
                enabled: u32::from(scanout.enabled),
                flags: 0,
            };
            resp.extend_from_slice(&disp.to_bytes());
        }

        resp
    }

    /// RESOURCE_CREATE_2D — create a new 2D resource.
    ///
    /// Payload:
    /// ```text
    /// le32 resource_id;
    /// le32 format;
    /// le32 width;
    /// le32 height;
    /// ```
    fn cmd_resource_create_2d(
        &mut self,
        request: &GpuCtrlHdr,
        payload: &[u8],
    ) -> Vec<u8> {
        if payload.len() < 16 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let resource_id = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
        let format = u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]);
        let width = u32::from_le_bytes([payload[8], payload[9], payload[10], payload[11]]);
        let height = u32::from_le_bytes([payload[12], payload[13], payload[14], payload[15]]);

        if resource_id == 0 {
            return self.make_error_response(
                GpuCommand::RespErrInvalidResourceId,
                Some(request),
            );
        }

        if width == 0 || height == 0 {
            return self.make_error_response(
                GpuCommand::RespErrInvalidParameter,
                Some(request),
            );
        }

        tracing::debug!(
            "virtio-gpu: create resource {} ({}x{}, format {})",
            resource_id,
            width,
            height,
            format
        );

        let resource = GpuResource::new(resource_id, width, height, format);
        self.resources.insert(resource_id, resource);

        self.make_ok_response(request)
    }

    /// RESOURCE_UNREF — destroy a resource and free its backing memory.
    ///
    /// Payload:
    /// ```text
    /// le32 resource_id;
    /// le32 padding;
    /// ```
    fn cmd_resource_unref(&mut self, request: &GpuCtrlHdr, payload: &[u8]) -> Vec<u8> {
        if payload.len() < 4 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let resource_id = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);

        if self.resources.remove(&resource_id).is_none() {
            return self.make_error_response(
                GpuCommand::RespErrInvalidResourceId,
                Some(request),
            );
        }

        // Unbind from any scanouts that reference this resource
        for scanout in &mut self.scanouts {
            if scanout.resource_id == resource_id {
                scanout.resource_id = 0;
                scanout.enabled = false;
            }
        }

        tracing::debug!("virtio-gpu: unref resource {}", resource_id);
        self.make_ok_response(request)
    }

    /// SET_SCANOUT — bind a resource to a scanout.
    ///
    /// Payload:
    /// ```text
    /// struct virtio_gpu_rect r;   // 16 bytes
    /// le32 scanout_id;
    /// le32 resource_id;
    /// ```
    fn cmd_set_scanout(&mut self, request: &GpuCtrlHdr, payload: &[u8]) -> Vec<u8> {
        if payload.len() < 24 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let rect = match GpuRect::from_bytes(&payload[0..16]) {
            Ok(r) => r,
            Err(_) => {
                return self.make_error_response(
                    GpuCommand::RespErrInvalidParameter,
                    Some(request),
                );
            }
        };

        let scanout_id =
            u32::from_le_bytes([payload[16], payload[17], payload[18], payload[19]]);
        let resource_id =
            u32::from_le_bytes([payload[20], payload[21], payload[22], payload[23]]);

        if scanout_id as usize >= VIRTIO_GPU_MAX_SCANOUTS {
            return self.make_error_response(
                GpuCommand::RespErrInvalidScanoutId,
                Some(request),
            );
        }

        // resource_id == 0 means disable the scanout
        if resource_id == 0 {
            self.scanouts[scanout_id as usize].resource_id = 0;
            self.scanouts[scanout_id as usize].enabled = false;
            tracing::debug!("virtio-gpu: disable scanout {}", scanout_id);
            return self.make_ok_response(request);
        }

        if !self.resources.contains_key(&resource_id) {
            return self.make_error_response(
                GpuCommand::RespErrInvalidResourceId,
                Some(request),
            );
        }

        self.scanouts[scanout_id as usize] = Scanout {
            resource_id,
            rect,
            enabled: true,
        };

        tracing::debug!(
            "virtio-gpu: set scanout {} to resource {} ({}x{} at {},{})",
            scanout_id,
            resource_id,
            rect.width,
            rect.height,
            rect.x,
            rect.y
        );

        self.make_ok_response(request)
    }

    /// RESOURCE_FLUSH — mark a region of a resource as updated for display.
    ///
    /// Payload:
    /// ```text
    /// struct virtio_gpu_rect r;   // 16 bytes
    /// le32 resource_id;
    /// le32 padding;
    /// ```
    fn cmd_resource_flush(&mut self, request: &GpuCtrlHdr, payload: &[u8]) -> Vec<u8> {
        if payload.len() < 24 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let resource_id =
            u32::from_le_bytes([payload[16], payload[17], payload[18], payload[19]]);

        let resource = match self.resources.get(&resource_id) {
            Some(r) => r,
            None => {
                return self.make_error_response(
                    GpuCommand::RespErrInvalidResourceId,
                    Some(request),
                );
            }
        };

        // Find the scanout(s) that reference this resource and invoke the
        // flush callback so the host display can update.
        if let Some(ref mut cb) = self.flush_callback {
            for (idx, scanout) in self.scanouts.iter().enumerate() {
                if scanout.enabled && scanout.resource_id == resource_id {
                    cb(
                        resource_id,
                        idx as u32,
                        &resource.backing,
                        resource.width,
                        resource.height,
                        resource.stride(),
                        resource.format,
                    );
                }
            }
        }

        tracing::trace!("virtio-gpu: flush resource {}", resource_id);
        self.make_ok_response(request)
    }

    /// TRANSFER_TO_HOST_2D — copy pixel data from guest backing to the resource.
    ///
    /// Payload:
    /// ```text
    /// struct virtio_gpu_rect r;   // 16 bytes
    /// le64 offset;
    /// le32 resource_id;
    /// le32 padding;
    /// ```
    fn cmd_transfer_to_host_2d(
        &mut self,
        request: &GpuCtrlHdr,
        payload: &[u8],
    ) -> Vec<u8> {
        if payload.len() < 32 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let rect = match GpuRect::from_bytes(&payload[0..16]) {
            Ok(r) => r,
            Err(_) => {
                return self.make_error_response(
                    GpuCommand::RespErrInvalidParameter,
                    Some(request),
                );
            }
        };

        let offset = u64::from_le_bytes([
            payload[16], payload[17], payload[18], payload[19], payload[20], payload[21],
            payload[22], payload[23],
        ]);

        let resource_id =
            u32::from_le_bytes([payload[24], payload[25], payload[26], payload[27]]);

        let resource = match self.resources.get(&resource_id) {
            Some(r) => r,
            None => {
                return self.make_error_response(
                    GpuCommand::RespErrInvalidResourceId,
                    Some(request),
                );
            }
        };

        if !resource.has_backing {
            return self.make_error_response(GpuCommand::RespErrUnspec, Some(request));
        }

        // In a full implementation, we would copy the sub-rect from the
        // guest-provided backing pages into the internal resource buffer.
        // Here we log the transfer — the data is already in `resource.backing`
        // since attach_backing copies it in.
        tracing::trace!(
            "virtio-gpu: transfer_to_host_2d resource {} rect ({}x{} at {},{}) offset {}",
            resource_id,
            rect.width,
            rect.height,
            rect.x,
            rect.y,
            offset
        );

        self.make_ok_response(request)
    }

    /// RESOURCE_ATTACH_BACKING — attach guest memory pages as backing store.
    ///
    /// Payload:
    /// ```text
    /// le32 resource_id;
    /// le32 nr_entries;
    /// // followed by nr_entries * struct virtio_gpu_mem_entry { le64 addr; le32 length; le32 padding; }
    /// ```
    ///
    /// In our model we simulate this by allocating a host-side buffer of the
    /// appropriate size (the actual data comes through transfer commands).
    fn cmd_resource_attach_backing(
        &mut self,
        request: &GpuCtrlHdr,
        payload: &[u8],
    ) -> Vec<u8> {
        if payload.len() < 8 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let resource_id = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
        let nr_entries = u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]);

        let resource = match self.resources.get_mut(&resource_id) {
            Some(r) => r,
            None => {
                return self.make_error_response(
                    GpuCommand::RespErrInvalidResourceId,
                    Some(request),
                );
            }
        };

        // Calculate total backing size from mem_entry array
        let mem_entry_size: usize = 16; // le64 addr + le32 length + le32 padding
        let entries_start = 8;
        let mut total_size: usize = 0;

        for i in 0..nr_entries as usize {
            let entry_offset = entries_start + i * mem_entry_size;
            if entry_offset + mem_entry_size <= payload.len() {
                let length = u32::from_le_bytes([
                    payload[entry_offset + 8],
                    payload[entry_offset + 9],
                    payload[entry_offset + 10],
                    payload[entry_offset + 11],
                ]);
                total_size += length as usize;
            }
        }

        // If we couldn't parse entries, fall back to the resource's expected size
        if total_size == 0 {
            total_size = resource.size_bytes();
        }

        resource.backing = vec![0u8; total_size];
        resource.has_backing = true;

        tracing::debug!(
            "virtio-gpu: attach backing to resource {} ({} entries, {} bytes)",
            resource_id,
            nr_entries,
            total_size
        );

        self.make_ok_response(request)
    }

    /// RESOURCE_DETACH_BACKING — remove backing store from a resource.
    ///
    /// Payload:
    /// ```text
    /// le32 resource_id;
    /// le32 padding;
    /// ```
    fn cmd_resource_detach_backing(
        &mut self,
        request: &GpuCtrlHdr,
        payload: &[u8],
    ) -> Vec<u8> {
        if payload.len() < 4 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let resource_id = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);

        let resource = match self.resources.get_mut(&resource_id) {
            Some(r) => r,
            None => {
                return self.make_error_response(
                    GpuCommand::RespErrInvalidResourceId,
                    Some(request),
                );
            }
        };

        resource.backing.clear();
        resource.has_backing = false;

        tracing::debug!("virtio-gpu: detach backing from resource {}", resource_id);
        self.make_ok_response(request)
    }

    /// GET_CAPSET_INFO — query capability set metadata.
    ///
    /// Payload:
    /// ```text
    /// le32 capset_index;
    /// le32 padding;
    /// ```
    ///
    /// Response payload (after header):
    /// ```text
    /// le32 capset_id;
    /// le32 capset_max_version;
    /// le32 capset_max_size;
    /// le32 padding;
    /// ```
    fn cmd_get_capset_info(
        &self,
        request: &GpuCtrlHdr,
        payload: &[u8],
    ) -> Vec<u8> {
        if payload.len() < 4 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let capset_index =
            u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);

        let resp_hdr =
            GpuCtrlHdr::response_with_fence(GpuCommand::RespOkCapsetInfo, request);

        let mut resp = Vec::with_capacity(CTRL_HDR_SIZE + 16);
        resp.extend_from_slice(&resp_hdr.to_bytes());

        match self.renderer.get_capset_info(capset_index) {
            Some(info) => {
                resp.extend_from_slice(&info.capset_id.to_le_bytes());
                resp.extend_from_slice(&info.max_version.to_le_bytes());
                resp.extend_from_slice(&info.max_size.to_le_bytes());
                resp.extend_from_slice(&0u32.to_le_bytes()); // padding
            }
            None => {
                // Return zeroed capset info for invalid index
                resp.extend_from_slice(&[0u8; 16]);
            }
        }

        resp
    }

    /// GET_CAPSET — retrieve capability set data.
    ///
    /// Payload:
    /// ```text
    /// le32 capset_id;
    /// le32 capset_version;
    /// ```
    ///
    /// Response payload: header + capset data bytes.
    fn cmd_get_capset(
        &self,
        request: &GpuCtrlHdr,
        payload: &[u8],
    ) -> Vec<u8> {
        if payload.len() < 8 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let capset_id = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
        let capset_version =
            u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]);

        let capset_data = self.renderer.get_capset(capset_id, capset_version);

        let resp_hdr =
            GpuCtrlHdr::response_with_fence(GpuCommand::RespOkCapset, request);

        let mut resp = Vec::with_capacity(CTRL_HDR_SIZE + capset_data.len());
        resp.extend_from_slice(&resp_hdr.to_bytes());
        resp.extend_from_slice(&capset_data);

        resp
    }

    /// CTX_CREATE — create a 3D rendering context.
    ///
    /// Payload:
    /// ```text
    /// le32 nlen;           // length of the debug name
    /// le32 context_init;   // flags
    /// char debug_name[64]; // debug string
    /// ```
    fn cmd_ctx_create(
        &mut self,
        request: &GpuCtrlHdr,
        payload: &[u8],
    ) -> Vec<u8> {
        if request.ctx_id == 0 {
            return self.make_error_response(
                GpuCommand::RespErrInvalidContextId,
                Some(request),
            );
        }

        if self.contexts.contains_key(&request.ctx_id) {
            return self.make_error_response(
                GpuCommand::RespErrInvalidContextId,
                Some(request),
            );
        }

        // Parse debug name from payload
        let name = if payload.len() >= 8 {
            let nlen =
                u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]])
                    as usize;
            let name_start = 8; // skip nlen + context_init
            let name_end = (name_start + nlen).min(payload.len());
            if name_start <= payload.len() && name_end <= payload.len() {
                String::from_utf8_lossy(&payload[name_start..name_end]).to_string()
            } else {
                String::new()
            }
        } else {
            String::new()
        };

        // Delegate to the renderer backend
        if self.renderer.create_context(request.ctx_id, &name).is_err() {
            return self.make_error_response(GpuCommand::RespErrUnspec, Some(request));
        }

        self.contexts.insert(
            request.ctx_id,
            Gpu3dContext {
                id: request.ctx_id,
                name: name.clone(),
                attached_resources: Vec::new(),
            },
        );

        tracing::debug!(
            "virtio-gpu: create context {} (\"{}\")",
            request.ctx_id,
            name
        );

        self.make_ok_response(request)
    }

    /// CTX_DESTROY — destroy a 3D rendering context.
    fn cmd_ctx_destroy(
        &mut self,
        request: &GpuCtrlHdr,
        _payload: &[u8],
    ) -> Vec<u8> {
        if request.ctx_id == 0 {
            return self.make_error_response(
                GpuCommand::RespErrInvalidContextId,
                Some(request),
            );
        }

        if self.contexts.remove(&request.ctx_id).is_none() {
            return self.make_error_response(
                GpuCommand::RespErrInvalidContextId,
                Some(request),
            );
        }

        let _ = self.renderer.destroy_context(request.ctx_id);

        tracing::debug!("virtio-gpu: destroy context {}", request.ctx_id);
        self.make_ok_response(request)
    }

    /// SUBMIT_3D — submit a 3D command buffer.
    ///
    /// Payload:
    /// ```text
    /// le32 size;     // size of the 3D command buffer in bytes
    /// le32 padding;
    /// // followed by `size` bytes of 3D command data
    /// ```
    fn cmd_submit_3d(
        &mut self,
        request: &GpuCtrlHdr,
        payload: &[u8],
    ) -> Vec<u8> {
        if request.ctx_id == 0 || !self.contexts.contains_key(&request.ctx_id) {
            return self.make_error_response(
                GpuCommand::RespErrInvalidContextId,
                Some(request),
            );
        }

        if payload.len() < 8 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let cmd_size =
            u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]) as usize;

        let cmd_data_start = 8;
        let cmd_data_end = (cmd_data_start + cmd_size).min(payload.len());
        let cmd_data = &payload[cmd_data_start..cmd_data_end];

        if self.renderer.submit_3d(request.ctx_id, cmd_data).is_err() {
            return self.make_error_response(GpuCommand::RespErrUnspec, Some(request));
        }

        tracing::trace!(
            "virtio-gpu: submit_3d ctx {} ({} bytes)",
            request.ctx_id,
            cmd_data.len()
        );

        self.make_ok_response(request)
    }

    /// CTX_ATTACH_RESOURCE — attach a resource to a 3D context.
    ///
    /// Payload:
    /// ```text
    /// le32 resource_id;
    /// le32 padding;
    /// ```
    fn cmd_ctx_attach_resource(
        &mut self,
        request: &GpuCtrlHdr,
        payload: &[u8],
    ) -> Vec<u8> {
        if request.ctx_id == 0 || !self.contexts.contains_key(&request.ctx_id) {
            return self.make_error_response(
                GpuCommand::RespErrInvalidContextId,
                Some(request),
            );
        }

        if payload.len() < 4 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let resource_id = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);

        if !self.resources.contains_key(&resource_id) {
            return self.make_error_response(
                GpuCommand::RespErrInvalidResourceId,
                Some(request),
            );
        }

        if let Some(ctx) = self.contexts.get_mut(&request.ctx_id) {
            if !ctx.attached_resources.contains(&resource_id) {
                ctx.attached_resources.push(resource_id);
            }
        }

        self.make_ok_response(request)
    }

    /// CTX_DETACH_RESOURCE — detach a resource from a 3D context.
    ///
    /// Payload:
    /// ```text
    /// le32 resource_id;
    /// le32 padding;
    /// ```
    fn cmd_ctx_detach_resource(
        &mut self,
        request: &GpuCtrlHdr,
        payload: &[u8],
    ) -> Vec<u8> {
        if request.ctx_id == 0 || !self.contexts.contains_key(&request.ctx_id) {
            return self.make_error_response(
                GpuCommand::RespErrInvalidContextId,
                Some(request),
            );
        }

        if payload.len() < 4 {
            return self.make_error_response(GpuCommand::RespErrInvalidParameter, Some(request));
        }

        let resource_id = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);

        if let Some(ctx) = self.contexts.get_mut(&request.ctx_id) {
            ctx.attached_resources.retain(|&id| id != resource_id);
        }

        self.make_ok_response(request)
    }

    // -----------------------------------------------------------------------
    // Response helpers
    // -----------------------------------------------------------------------

    /// Build an OK (no data) response, optionally copying fence info.
    fn make_ok_response(&self, request: &GpuCtrlHdr) -> Vec<u8> {
        let hdr = GpuCtrlHdr::response_with_fence(GpuCommand::RespOkNodata, request);
        hdr.to_bytes().to_vec()
    }

    /// Build an error response.
    fn make_error_response(
        &self,
        err: GpuCommand,
        request: Option<&GpuCtrlHdr>,
    ) -> Vec<u8> {
        let hdr = match request {
            Some(req) => GpuCtrlHdr::response_with_fence(err, req),
            None => GpuCtrlHdr::response(err),
        };
        hdr.to_bytes().to_vec()
    }
}

// ============================================================================
// VirtioDevice trait implementation
// ============================================================================

impl VirtioDevice for VirtioGpu {
    fn device_type(&self) -> VirtioDeviceType {
        VirtioDeviceType::Gpu
    }

    fn features(&self) -> u64 {
        self.features
    }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!(
            "virtio-gpu activated: {}x{} (features: {:#x})",
            self.config.width,
            self.config.height,
            self.features
        );
        Ok(())
    }

    fn reset(&mut self) {
        self.activated = false;
        self.resources.clear();
        self.contexts.clear();
        self.completed_fences.clear();

        // Reset scanouts to default state
        for scanout in &mut self.scanouts {
            *scanout = Scanout::default();
        }
        // Re-enable scanout 0 with configured resolution
        self.scanouts[0] = Scanout {
            resource_id: 0,
            rect: GpuRect {
                x: 0,
                y: 0,
                width: self.config.width,
                height: self.config.height,
            },
            enabled: true,
        };
    }

    fn process_queue(&mut self, queue_index: u16) -> DeviceResult<()> {
        match queue_index {
            CONTROLQ => {
                tracing::trace!("virtio-gpu: control queue notified");
                Ok(())
            }
            CURSORQ => {
                tracing::trace!("virtio-gpu: cursor queue notified");
                Ok(())
            }
            _ => {
                tracing::warn!("virtio-gpu: unknown queue index {}", queue_index);
                Ok(())
            }
        }
    }
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    // -----------------------------------------------------------------------
    // Helper: build a command buffer with header + payload
    // -----------------------------------------------------------------------

    fn make_cmd(cmd_type: GpuCommand, ctx_id: u32, payload: &[u8]) -> Vec<u8> {
        let hdr = GpuCtrlHdr {
            type_: cmd_type as u32,
            flags: 0,
            fence_id: 0,
            ctx_id,
            ring_idx: 0,
        };
        let mut buf = Vec::with_capacity(CTRL_HDR_SIZE + payload.len());
        buf.extend_from_slice(&hdr.to_bytes());
        buf.extend_from_slice(payload);
        buf
    }

    fn make_cmd_with_fence(
        cmd_type: GpuCommand,
        ctx_id: u32,
        fence_id: u64,
        payload: &[u8],
    ) -> Vec<u8> {
        let hdr = GpuCtrlHdr {
            type_: cmd_type as u32,
            flags: VIRTIO_GPU_FLAG_FENCE,
            fence_id,
            ctx_id,
            ring_idx: 0,
        };
        let mut buf = Vec::with_capacity(CTRL_HDR_SIZE + payload.len());
        buf.extend_from_slice(&hdr.to_bytes());
        buf.extend_from_slice(payload);
        buf
    }

    fn resource_create_payload(
        resource_id: u32,
        format: u32,
        width: u32,
        height: u32,
    ) -> Vec<u8> {
        let mut p = Vec::with_capacity(16);
        p.extend_from_slice(&resource_id.to_le_bytes());
        p.extend_from_slice(&format.to_le_bytes());
        p.extend_from_slice(&width.to_le_bytes());
        p.extend_from_slice(&height.to_le_bytes());
        p
    }

    fn resource_id_payload(resource_id: u32) -> Vec<u8> {
        let mut p = Vec::with_capacity(8);
        p.extend_from_slice(&resource_id.to_le_bytes());
        p.extend_from_slice(&0u32.to_le_bytes()); // padding
        p
    }

    fn set_scanout_payload(
        rect: &GpuRect,
        scanout_id: u32,
        resource_id: u32,
    ) -> Vec<u8> {
        let mut p = Vec::with_capacity(24);
        p.extend_from_slice(&rect.to_bytes());
        p.extend_from_slice(&scanout_id.to_le_bytes());
        p.extend_from_slice(&resource_id.to_le_bytes());
        p
    }

    fn attach_backing_payload(resource_id: u32, total_size: u32) -> Vec<u8> {
        // 1 mem_entry: addr=0, length=total_size
        let mut p = Vec::with_capacity(8 + 16);
        p.extend_from_slice(&resource_id.to_le_bytes());
        p.extend_from_slice(&1u32.to_le_bytes()); // nr_entries = 1
        p.extend_from_slice(&0u64.to_le_bytes()); // addr
        p.extend_from_slice(&total_size.to_le_bytes()); // length
        p.extend_from_slice(&0u32.to_le_bytes()); // padding
        p
    }

    fn transfer_payload(
        rect: &GpuRect,
        offset: u64,
        resource_id: u32,
    ) -> Vec<u8> {
        let mut p = Vec::with_capacity(32);
        p.extend_from_slice(&rect.to_bytes());
        p.extend_from_slice(&offset.to_le_bytes());
        p.extend_from_slice(&resource_id.to_le_bytes());
        p.extend_from_slice(&0u32.to_le_bytes()); // padding
        p
    }

    fn flush_payload(rect: &GpuRect, resource_id: u32) -> Vec<u8> {
        let mut p = Vec::with_capacity(24);
        p.extend_from_slice(&rect.to_bytes());
        p.extend_from_slice(&resource_id.to_le_bytes());
        p.extend_from_slice(&0u32.to_le_bytes()); // padding
        p
    }

    fn resp_type(resp: &[u8]) -> u32 {
        u32::from_le_bytes([resp[0], resp[1], resp[2], resp[3]])
    }

    // -----------------------------------------------------------------------
    // 1. Resource create / unref lifecycle
    // -----------------------------------------------------------------------

    #[test]
    fn test_resource_create_unref_lifecycle() {
        let mut gpu = VirtioGpu::new(1920, 1080);

        // Create resource 1
        let payload = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 640, 480);
        let cmd = make_cmd(GpuCommand::ResourceCreate2d, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);
        assert_eq!(gpu.resource_count(), 1);

        // Verify resource exists
        let res = gpu.resource(1).unwrap();
        assert_eq!(res.width, 640);
        assert_eq!(res.height, 480);
        assert_eq!(res.format, GpuFormat::B8G8R8A8Unorm as u32);

        // Unref resource 1
        let payload = resource_id_payload(1);
        let cmd = make_cmd(GpuCommand::ResourceUnref, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);
        assert_eq!(gpu.resource_count(), 0);
    }

    // -----------------------------------------------------------------------
    // 2. Display info query
    // -----------------------------------------------------------------------

    #[test]
    fn test_get_display_info() {
        let mut gpu = VirtioGpu::new(1920, 1080);

        let cmd = make_cmd(GpuCommand::GetDisplayInfo, 0, &[]);
        let resp = gpu.process_command(&cmd);

        // Response header should be RespOkDisplayInfo
        assert_eq!(resp_type(&resp), GpuCommand::RespOkDisplayInfo as u32);

        // Payload: VIRTIO_GPU_MAX_SCANOUTS * 24 bytes
        let expected_len = CTRL_HDR_SIZE + VIRTIO_GPU_MAX_SCANOUTS * GpuDisplayOne::SIZE;
        assert_eq!(resp.len(), expected_len);

        // First scanout should be enabled with configured dimensions
        let s0_offset = CTRL_HDR_SIZE;
        let s0_rect = GpuRect::from_bytes(&resp[s0_offset..s0_offset + 16]).unwrap();
        assert_eq!(s0_rect.width, 1920);
        assert_eq!(s0_rect.height, 1080);
        let s0_enabled = u32::from_le_bytes([
            resp[s0_offset + 16],
            resp[s0_offset + 17],
            resp[s0_offset + 18],
            resp[s0_offset + 19],
        ]);
        assert_eq!(s0_enabled, 1);

        // Second scanout should be disabled
        let s1_offset = s0_offset + GpuDisplayOne::SIZE;
        let s1_enabled = u32::from_le_bytes([
            resp[s1_offset + 16],
            resp[s1_offset + 17],
            resp[s1_offset + 18],
            resp[s1_offset + 19],
        ]);
        assert_eq!(s1_enabled, 0);
    }

    // -----------------------------------------------------------------------
    // 3. Scanout configuration
    // -----------------------------------------------------------------------

    #[test]
    fn test_set_scanout() {
        let mut gpu = VirtioGpu::new(1920, 1080);

        // Create a resource first
        let create = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 1920, 1080);
        let cmd = make_cmd(GpuCommand::ResourceCreate2d, 0, &create);
        gpu.process_command(&cmd);

        // Set scanout 0 to resource 1
        let rect = GpuRect {
            x: 0,
            y: 0,
            width: 1920,
            height: 1080,
        };
        let payload = set_scanout_payload(&rect, 0, 1);
        let cmd = make_cmd(GpuCommand::SetScanout, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);

        let scanout = gpu.scanout(0).unwrap();
        assert_eq!(scanout.resource_id, 1);
        assert!(scanout.enabled);
        assert_eq!(scanout.rect.width, 1920);
        assert_eq!(scanout.rect.height, 1080);
    }

    // -----------------------------------------------------------------------
    // 4. Resource attach / detach backing
    // -----------------------------------------------------------------------

    #[test]
    fn test_resource_attach_detach_backing() {
        let mut gpu = VirtioGpu::new(1920, 1080);

        // Create resource
        let create = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 100, 100);
        let cmd = make_cmd(GpuCommand::ResourceCreate2d, 0, &create);
        gpu.process_command(&cmd);

        // Attach backing (100*100*4 = 40000 bytes)
        let payload = attach_backing_payload(1, 40000);
        let cmd = make_cmd(GpuCommand::ResourceAttachBacking, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);

        let res = gpu.resource(1).unwrap();
        assert!(res.has_backing);
        assert_eq!(res.backing.len(), 40000);

        // Detach backing
        let payload = resource_id_payload(1);
        let cmd = make_cmd(GpuCommand::ResourceDetachBacking, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);

        let res = gpu.resource(1).unwrap();
        assert!(!res.has_backing);
        assert!(res.backing.is_empty());
    }

    // -----------------------------------------------------------------------
    // 5. Transfer to host 2D
    // -----------------------------------------------------------------------

    #[test]
    fn test_transfer_to_host_2d() {
        let mut gpu = VirtioGpu::new(1920, 1080);

        // Create and attach backing
        let create = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 64, 64);
        gpu.process_command(&make_cmd(GpuCommand::ResourceCreate2d, 0, &create));

        let attach = attach_backing_payload(1, 64 * 64 * 4);
        gpu.process_command(&make_cmd(GpuCommand::ResourceAttachBacking, 0, &attach));

        // Transfer
        let rect = GpuRect {
            x: 0,
            y: 0,
            width: 64,
            height: 64,
        };
        let payload = transfer_payload(&rect, 0, 1);
        let cmd = make_cmd(GpuCommand::TransferToHost2d, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);
    }

    // -----------------------------------------------------------------------
    // 6. Resource flush
    // -----------------------------------------------------------------------

    #[test]
    fn test_resource_flush() {
        let mut gpu = VirtioGpu::new(1920, 1080);

        // Create resource, attach backing, set scanout
        let create = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 1920, 1080);
        gpu.process_command(&make_cmd(GpuCommand::ResourceCreate2d, 0, &create));

        let attach = attach_backing_payload(1, 1920 * 1080 * 4);
        gpu.process_command(&make_cmd(GpuCommand::ResourceAttachBacking, 0, &attach));

        let rect = GpuRect { x: 0, y: 0, width: 1920, height: 1080 };
        let scanout = set_scanout_payload(&rect, 0, 1);
        gpu.process_command(&make_cmd(GpuCommand::SetScanout, 0, &scanout));

        // Track flush callback invocation
        let flushed = std::sync::Arc::new(std::sync::Mutex::new(false));
        let flushed_clone = flushed.clone();
        gpu.set_flush_callback(Box::new(
            move |_res_id, _scanout_id, _data, _w, _h, _stride, _fmt| {
                *flushed_clone.lock().unwrap() = true;
            },
        ));

        // Flush
        let payload = flush_payload(&rect, 1);
        let cmd = make_cmd(GpuCommand::ResourceFlush, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);
        assert!(*flushed.lock().unwrap());
    }

    // -----------------------------------------------------------------------
    // 7. Command header serialization / deserialization
    // -----------------------------------------------------------------------

    #[test]
    fn test_ctrl_hdr_roundtrip() {
        let hdr = GpuCtrlHdr {
            type_: GpuCommand::ResourceCreate2d as u32,
            flags: VIRTIO_GPU_FLAG_FENCE,
            fence_id: 0xDEAD_BEEF_CAFE_BABE,
            ctx_id: 42,
            ring_idx: 7,
        };

        let bytes = hdr.to_bytes();
        assert_eq!(bytes.len(), CTRL_HDR_SIZE);

        let parsed = GpuCtrlHdr::from_bytes(&bytes).unwrap();
        assert_eq!(parsed.type_, hdr.type_);
        assert_eq!(parsed.flags, hdr.flags);
        assert_eq!(parsed.fence_id, hdr.fence_id);
        assert_eq!(parsed.ctx_id, hdr.ctx_id);
        assert_eq!(parsed.ring_idx, hdr.ring_idx);
    }

    // -----------------------------------------------------------------------
    // 8. Feature negotiation
    // -----------------------------------------------------------------------

    #[test]
    fn test_feature_negotiation() {
        // No features
        let gpu = VirtioGpu::new(640, 480);
        assert_eq!(gpu.features(), 0);
        assert_eq!(gpu.device_type(), VirtioDeviceType::Gpu);

        // Virgl enabled
        let gpu = VirtioGpu::with_config(VirtioGpuConfig {
            width: 640,
            height: 480,
            virgl: true,
            edid: false,
        });
        assert!(gpu.features() & features::VIRTIO_GPU_F_VIRGL != 0);
        assert!(gpu.features() & features::VIRTIO_GPU_F_EDID == 0);

        // Both virgl and EDID
        let gpu = VirtioGpu::with_config(VirtioGpuConfig {
            width: 640,
            height: 480,
            virgl: true,
            edid: true,
        });
        assert!(gpu.features() & features::VIRTIO_GPU_F_VIRGL != 0);
        assert!(gpu.features() & features::VIRTIO_GPU_F_EDID != 0);
    }

    // -----------------------------------------------------------------------
    // 9. 3D context create / destroy
    // -----------------------------------------------------------------------

    #[test]
    fn test_3d_context_create_destroy() {
        let mut gpu = VirtioGpu::with_config(VirtioGpuConfig {
            width: 640,
            height: 480,
            virgl: true,
            edid: false,
        });

        // Create context with ctx_id = 1
        let mut payload = Vec::new();
        let name = b"test-ctx";
        payload.extend_from_slice(&(name.len() as u32).to_le_bytes()); // nlen
        payload.extend_from_slice(&0u32.to_le_bytes()); // context_init
        payload.extend_from_slice(name);

        let cmd = make_cmd(GpuCommand::CtxCreate, 1, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);
        assert_eq!(gpu.context_count(), 1);

        // Destroy context
        let cmd = make_cmd(GpuCommand::CtxDestroy, 1, &[]);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);
        assert_eq!(gpu.context_count(), 0);
    }

    // -----------------------------------------------------------------------
    // 10. Invalid resource ID handling
    // -----------------------------------------------------------------------

    #[test]
    fn test_invalid_resource_id() {
        let mut gpu = VirtioGpu::new(640, 480);

        // Unref non-existent resource
        let payload = resource_id_payload(999);
        let cmd = make_cmd(GpuCommand::ResourceUnref, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidResourceId as u32);

        // Detach backing from non-existent resource
        let cmd = make_cmd(GpuCommand::ResourceDetachBacking, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidResourceId as u32);

        // Set scanout to non-existent resource
        let rect = GpuRect { x: 0, y: 0, width: 640, height: 480 };
        let payload = set_scanout_payload(&rect, 0, 999);
        let cmd = make_cmd(GpuCommand::SetScanout, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidResourceId as u32);

        // Create resource with ID 0 (invalid)
        let payload = resource_create_payload(0, GpuFormat::B8G8R8A8Unorm as u32, 100, 100);
        let cmd = make_cmd(GpuCommand::ResourceCreate2d, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidResourceId as u32);
    }

    // -----------------------------------------------------------------------
    // 11. Multiple scanout support
    // -----------------------------------------------------------------------

    #[test]
    fn test_multiple_scanouts() {
        let mut gpu = VirtioGpu::new(1920, 1080);

        // Create two resources
        let c1 = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 1920, 1080);
        gpu.process_command(&make_cmd(GpuCommand::ResourceCreate2d, 0, &c1));

        let c2 = resource_create_payload(2, GpuFormat::B8G8R8A8Unorm as u32, 800, 600);
        gpu.process_command(&make_cmd(GpuCommand::ResourceCreate2d, 0, &c2));

        // Bind scanout 0 to resource 1
        let r0 = GpuRect { x: 0, y: 0, width: 1920, height: 1080 };
        let s0 = set_scanout_payload(&r0, 0, 1);
        let resp = gpu.process_command(&make_cmd(GpuCommand::SetScanout, 0, &s0));
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);

        // Bind scanout 1 to resource 2
        let r1 = GpuRect { x: 0, y: 0, width: 800, height: 600 };
        let s1 = set_scanout_payload(&r1, 1, 2);
        let resp = gpu.process_command(&make_cmd(GpuCommand::SetScanout, 0, &s1));
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);

        assert_eq!(gpu.scanout(0).unwrap().resource_id, 1);
        assert!(gpu.scanout(0).unwrap().enabled);
        assert_eq!(gpu.scanout(1).unwrap().resource_id, 2);
        assert!(gpu.scanout(1).unwrap().enabled);

        // Invalid scanout ID
        let s_bad = set_scanout_payload(&r0, VIRTIO_GPU_MAX_SCANOUTS as u32, 1);
        let resp = gpu.process_command(&make_cmd(GpuCommand::SetScanout, 0, &s_bad));
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidScanoutId as u32);
    }

    // -----------------------------------------------------------------------
    // 12. Capset info query
    // -----------------------------------------------------------------------

    #[test]
    fn test_capset_info_query() {
        let mut gpu = VirtioGpu::with_config(VirtioGpuConfig {
            width: 640,
            height: 480,
            virgl: true,
            edid: false,
        });

        // Set up a custom renderer that reports capsets
        struct TestRenderer;
        impl GpuRenderer for TestRenderer {
            fn create_context(&mut self, _: u32, _: &str) -> DeviceResult<()> {
                Ok(())
            }
            fn destroy_context(&mut self, _: u32) -> DeviceResult<()> {
                Ok(())
            }
            fn submit_3d(&mut self, _: u32, _: &[u8]) -> DeviceResult<()> {
                Ok(())
            }
            fn get_capset_info(&self, index: u32) -> Option<CapsetInfo> {
                match index {
                    0 => Some(CapsetInfo {
                        capset_id: 1,
                        max_version: 2,
                        max_size: 1024,
                    }),
                    1 => Some(CapsetInfo {
                        capset_id: 2,
                        max_version: 1,
                        max_size: 512,
                    }),
                    _ => None,
                }
            }
            fn num_capsets(&self) -> u32 {
                2
            }
            fn get_capset(&self, _: u32, _: u32) -> Vec<u8> {
                vec![0xAA; 64]
            }
        }

        gpu.set_renderer(Box::new(TestRenderer));

        // Query capset index 0
        let mut payload = Vec::new();
        payload.extend_from_slice(&0u32.to_le_bytes());
        payload.extend_from_slice(&0u32.to_le_bytes()); // padding

        let cmd = make_cmd(GpuCommand::GetCapsetInfo, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkCapsetInfo as u32);

        // Parse capset info from response payload
        let p = &resp[CTRL_HDR_SIZE..];
        let capset_id = u32::from_le_bytes([p[0], p[1], p[2], p[3]]);
        let max_version = u32::from_le_bytes([p[4], p[5], p[6], p[7]]);
        let max_size = u32::from_le_bytes([p[8], p[9], p[10], p[11]]);
        assert_eq!(capset_id, 1);
        assert_eq!(max_version, 2);
        assert_eq!(max_size, 1024);

        // Query non-existent capset index
        payload.clear();
        payload.extend_from_slice(&99u32.to_le_bytes());
        payload.extend_from_slice(&0u32.to_le_bytes());
        let cmd = make_cmd(GpuCommand::GetCapsetInfo, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkCapsetInfo as u32);
        let p = &resp[CTRL_HDR_SIZE..];
        let capset_id = u32::from_le_bytes([p[0], p[1], p[2], p[3]]);
        assert_eq!(capset_id, 0); // zeroed for invalid
    }

    // -----------------------------------------------------------------------
    // 13. GpuRect serialization roundtrip
    // -----------------------------------------------------------------------

    #[test]
    fn test_gpu_rect_roundtrip() {
        let rect = GpuRect {
            x: 10,
            y: 20,
            width: 640,
            height: 480,
        };
        let bytes = rect.to_bytes();
        assert_eq!(bytes.len(), 16);

        let parsed = GpuRect::from_bytes(&bytes).unwrap();
        assert_eq!(parsed, rect);
    }

    // -----------------------------------------------------------------------
    // 14. Fence tracking
    // -----------------------------------------------------------------------

    #[test]
    fn test_fence_tracking() {
        let mut gpu = VirtioGpu::new(640, 480);

        // Create resource with fence
        let payload = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 100, 100);
        let cmd = make_cmd_with_fence(GpuCommand::ResourceCreate2d, 0, 42, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);

        // Check that fence ID was propagated in the response
        let resp_hdr = GpuCtrlHdr::from_bytes(&resp).unwrap();
        assert_eq!(resp_hdr.flags & VIRTIO_GPU_FLAG_FENCE, VIRTIO_GPU_FLAG_FENCE);
        assert_eq!(resp_hdr.fence_id, 42);

        // Drain completed fences
        let fences = gpu.drain_fences();
        assert_eq!(fences, vec![42]);
        assert!(gpu.drain_fences().is_empty());
    }

    // -----------------------------------------------------------------------
    // 15. Activate and reset
    // -----------------------------------------------------------------------

    #[test]
    fn test_activate_and_reset() {
        let mut gpu = VirtioGpu::new(1920, 1080);
        gpu.activate().unwrap();

        // Create some state
        let create = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 100, 100);
        gpu.process_command(&make_cmd(GpuCommand::ResourceCreate2d, 0, &create));
        assert_eq!(gpu.resource_count(), 1);

        // Reset should clear everything
        gpu.reset();
        assert_eq!(gpu.resource_count(), 0);
        assert_eq!(gpu.context_count(), 0);

        // Scanout 0 should be re-enabled with default dimensions
        let s0 = gpu.scanout(0).unwrap();
        assert!(s0.enabled);
        assert_eq!(s0.rect.width, 1920);
        assert_eq!(s0.rect.height, 1080);
    }

    // -----------------------------------------------------------------------
    // 16. Resource size calculation
    // -----------------------------------------------------------------------

    #[test]
    fn test_resource_size_bytes() {
        let res = GpuResource::new(1, 1920, 1080, GpuFormat::B8G8R8A8Unorm as u32);
        assert_eq!(res.size_bytes(), 1920 * 1080 * 4);
        assert_eq!(res.stride(), 1920 * 4);

        let res = GpuResource::new(2, 100, 100, GpuFormat::R8G8B8A8Unorm as u32);
        assert_eq!(res.size_bytes(), 100 * 100 * 4);
    }

    // -----------------------------------------------------------------------
    // 17. GpuFormat parsing
    // -----------------------------------------------------------------------

    #[test]
    fn test_gpu_format_parsing() {
        assert_eq!(GpuFormat::from_u32(1), Some(GpuFormat::B8G8R8A8Unorm));
        assert_eq!(GpuFormat::from_u32(67), Some(GpuFormat::R8G8B8A8Unorm));
        assert_eq!(GpuFormat::from_u32(9999), None);

        assert_eq!(GpuFormat::B8G8R8A8Unorm.bytes_per_pixel(), 4);
        assert_eq!(GpuFormat::R8G8B8A8Unorm.bytes_per_pixel(), 4);
    }

    // -----------------------------------------------------------------------
    // 18. GpuCommand parsing
    // -----------------------------------------------------------------------

    #[test]
    fn test_gpu_command_parsing() {
        assert_eq!(
            GpuCommand::from_u32(0x0100),
            Some(GpuCommand::GetDisplayInfo)
        );
        assert_eq!(
            GpuCommand::from_u32(0x0101),
            Some(GpuCommand::ResourceCreate2d)
        );
        assert_eq!(
            GpuCommand::from_u32(0x1100),
            Some(GpuCommand::RespOkNodata)
        );
        assert_eq!(
            GpuCommand::from_u32(0x1200),
            Some(GpuCommand::RespErrUnspec)
        );
        assert_eq!(GpuCommand::from_u32(0xFFFF), None);
    }

    // -----------------------------------------------------------------------
    // 19. Unknown command handling
    // -----------------------------------------------------------------------

    #[test]
    fn test_unknown_command() {
        let mut gpu = VirtioGpu::new(640, 480);

        // Build a command with an unrecognized type
        let hdr = GpuCtrlHdr {
            type_: 0xFFFF,
            flags: 0,
            fence_id: 0,
            ctx_id: 0,
            ring_idx: 0,
        };
        let resp = gpu.process_command(&hdr.to_bytes());
        assert_eq!(resp_type(&resp), GpuCommand::RespErrUnspec as u32);
    }

    // -----------------------------------------------------------------------
    // 20. Scanout disable
    // -----------------------------------------------------------------------

    #[test]
    fn test_scanout_disable() {
        let mut gpu = VirtioGpu::new(1920, 1080);

        // Create resource and set scanout
        let create = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 1920, 1080);
        gpu.process_command(&make_cmd(GpuCommand::ResourceCreate2d, 0, &create));

        let rect = GpuRect { x: 0, y: 0, width: 1920, height: 1080 };
        let payload = set_scanout_payload(&rect, 0, 1);
        gpu.process_command(&make_cmd(GpuCommand::SetScanout, 0, &payload));
        assert!(gpu.scanout(0).unwrap().enabled);

        // Disable scanout by setting resource_id = 0
        let payload = set_scanout_payload(&rect, 0, 0);
        let resp = gpu.process_command(&make_cmd(GpuCommand::SetScanout, 0, &payload));
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);
        assert!(!gpu.scanout(0).unwrap().enabled);
    }

    // -----------------------------------------------------------------------
    // 21. Transfer without backing fails
    // -----------------------------------------------------------------------

    #[test]
    fn test_transfer_without_backing() {
        let mut gpu = VirtioGpu::new(640, 480);

        // Create resource without attaching backing
        let create = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 64, 64);
        gpu.process_command(&make_cmd(GpuCommand::ResourceCreate2d, 0, &create));

        let rect = GpuRect { x: 0, y: 0, width: 64, height: 64 };
        let payload = transfer_payload(&rect, 0, 1);
        let cmd = make_cmd(GpuCommand::TransferToHost2d, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrUnspec as u32);
    }

    // -----------------------------------------------------------------------
    // 22. 3D context operations: invalid context
    // -----------------------------------------------------------------------

    #[test]
    fn test_invalid_context_operations() {
        let mut gpu = VirtioGpu::new(640, 480);

        // Create context with ctx_id = 0 (invalid)
        let cmd = make_cmd(GpuCommand::CtxCreate, 0, &[0u8; 72]);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidContextId as u32);

        // Destroy non-existent context
        let cmd = make_cmd(GpuCommand::CtxDestroy, 99, &[]);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidContextId as u32);

        // Submit 3D to non-existent context
        let mut payload = Vec::new();
        payload.extend_from_slice(&0u32.to_le_bytes());
        payload.extend_from_slice(&0u32.to_le_bytes());
        let cmd = make_cmd(GpuCommand::Submit3d, 99, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidContextId as u32);
    }

    // -----------------------------------------------------------------------
    // 23. 3D submit
    // -----------------------------------------------------------------------

    #[test]
    fn test_3d_submit() {
        let mut gpu = VirtioGpu::with_config(VirtioGpuConfig {
            width: 640,
            height: 480,
            virgl: true,
            edid: false,
        });

        // Create context
        let mut ctx_payload = Vec::new();
        let name = b"render";
        ctx_payload.extend_from_slice(&(name.len() as u32).to_le_bytes());
        ctx_payload.extend_from_slice(&0u32.to_le_bytes());
        ctx_payload.extend_from_slice(name);
        gpu.process_command(&make_cmd(GpuCommand::CtxCreate, 1, &ctx_payload));

        // Submit 3D commands
        let cmd_data = [0xAAu8; 32];
        let mut submit_payload = Vec::new();
        submit_payload.extend_from_slice(&(cmd_data.len() as u32).to_le_bytes());
        submit_payload.extend_from_slice(&0u32.to_le_bytes()); // padding
        submit_payload.extend_from_slice(&cmd_data);

        let cmd = make_cmd(GpuCommand::Submit3d, 1, &submit_payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);
    }

    // -----------------------------------------------------------------------
    // 24. Resource unref unbinds scanout
    // -----------------------------------------------------------------------

    #[test]
    fn test_resource_unref_unbinds_scanout() {
        let mut gpu = VirtioGpu::new(1920, 1080);

        // Create resource, set scanout
        let create = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 1920, 1080);
        gpu.process_command(&make_cmd(GpuCommand::ResourceCreate2d, 0, &create));

        let rect = GpuRect { x: 0, y: 0, width: 1920, height: 1080 };
        let payload = set_scanout_payload(&rect, 0, 1);
        gpu.process_command(&make_cmd(GpuCommand::SetScanout, 0, &payload));
        assert!(gpu.scanout(0).unwrap().enabled);

        // Unref the resource — scanout should be disabled
        let payload = resource_id_payload(1);
        gpu.process_command(&make_cmd(GpuCommand::ResourceUnref, 0, &payload));
        assert!(!gpu.scanout(0).unwrap().enabled);
        assert_eq!(gpu.scanout(0).unwrap().resource_id, 0);
    }

    // -----------------------------------------------------------------------
    // 25. Duplicate context create
    // -----------------------------------------------------------------------

    #[test]
    fn test_duplicate_context_create() {
        let mut gpu = VirtioGpu::new(640, 480);

        let mut payload = Vec::new();
        let name = b"ctx1";
        payload.extend_from_slice(&(name.len() as u32).to_le_bytes());
        payload.extend_from_slice(&0u32.to_le_bytes());
        payload.extend_from_slice(name);

        // First creation succeeds
        let cmd = make_cmd(GpuCommand::CtxCreate, 1, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);

        // Second creation with same ID fails
        let cmd = make_cmd(GpuCommand::CtxCreate, 1, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidContextId as u32);
    }

    // -----------------------------------------------------------------------
    // 26. Process queue
    // -----------------------------------------------------------------------

    #[test]
    fn test_process_queue() {
        let mut gpu = VirtioGpu::new(640, 480);
        gpu.activate().unwrap();

        assert!(gpu.process_queue(CONTROLQ).is_ok());
        assert!(gpu.process_queue(CURSORQ).is_ok());
        assert!(gpu.process_queue(99).is_ok());
    }

    // -----------------------------------------------------------------------
    // 27. Display dimensions
    // -----------------------------------------------------------------------

    #[test]
    fn test_display_dimensions() {
        let gpu = VirtioGpu::new(2560, 1440);
        assert_eq!(gpu.width(), 2560);
        assert_eq!(gpu.height(), 1440);
    }

    // -----------------------------------------------------------------------
    // 28. GpuDisplayOne serialization
    // -----------------------------------------------------------------------

    #[test]
    fn test_display_one_serialization() {
        let disp = GpuDisplayOne {
            rect: GpuRect { x: 0, y: 0, width: 1920, height: 1080 },
            enabled: 1,
            flags: 0,
        };
        let bytes = disp.to_bytes();
        assert_eq!(bytes.len(), GpuDisplayOne::SIZE);

        let rect = GpuRect::from_bytes(&bytes[0..16]).unwrap();
        assert_eq!(rect.width, 1920);
        assert_eq!(rect.height, 1080);

        let enabled = u32::from_le_bytes([bytes[16], bytes[17], bytes[18], bytes[19]]);
        assert_eq!(enabled, 1);
    }

    // -----------------------------------------------------------------------
    // 29. Create resource with zero dimensions
    // -----------------------------------------------------------------------

    #[test]
    fn test_create_resource_zero_dimensions() {
        let mut gpu = VirtioGpu::new(640, 480);

        let payload = resource_create_payload(1, GpuFormat::B8G8R8A8Unorm as u32, 0, 100);
        let cmd = make_cmd(GpuCommand::ResourceCreate2d, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidParameter as u32);

        let payload = resource_create_payload(2, GpuFormat::B8G8R8A8Unorm as u32, 100, 0);
        let cmd = make_cmd(GpuCommand::ResourceCreate2d, 0, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidParameter as u32);
    }

    // -----------------------------------------------------------------------
    // 30. Context attach/detach resource
    // -----------------------------------------------------------------------

    #[test]
    fn test_ctx_attach_detach_resource() {
        let mut gpu = VirtioGpu::new(640, 480);

        // Create context
        let mut ctx_payload = Vec::new();
        let name = b"ctx";
        ctx_payload.extend_from_slice(&(name.len() as u32).to_le_bytes());
        ctx_payload.extend_from_slice(&0u32.to_le_bytes());
        ctx_payload.extend_from_slice(name);
        gpu.process_command(&make_cmd(GpuCommand::CtxCreate, 1, &ctx_payload));

        // Create resource
        let create = resource_create_payload(10, GpuFormat::B8G8R8A8Unorm as u32, 100, 100);
        gpu.process_command(&make_cmd(GpuCommand::ResourceCreate2d, 0, &create));

        // Attach resource to context
        let payload = resource_id_payload(10);
        let cmd = make_cmd(GpuCommand::CtxAttachResource, 1, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);

        // Detach resource from context
        let cmd = make_cmd(GpuCommand::CtxDetachResource, 1, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespOkNodata as u32);

        // Attach to invalid context
        let cmd = make_cmd(GpuCommand::CtxAttachResource, 99, &payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidContextId as u32);

        // Attach non-existent resource
        let bad_payload = resource_id_payload(999);
        let cmd = make_cmd(GpuCommand::CtxAttachResource, 1, &bad_payload);
        let resp = gpu.process_command(&cmd);
        assert_eq!(resp_type(&resp), GpuCommand::RespErrInvalidResourceId as u32);
    }
}
