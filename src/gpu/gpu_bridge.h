#pragma once

#include "renderer.h"
#include "software_renderer.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace rex::gpu {

/// GPU bridge between virtio-gpu (Rust) and host renderer (C++)
///
/// The Rust virtio-gpu device calls into this bridge via FFI to:
/// - Create/destroy GPU resources
/// - Transfer pixel data from guest to host
/// - Set scanout configuration
/// - Submit 3D commands (forwarded to virglrenderer/Venus)
/// - Flush resources to the display
///
/// This bridge owns the IRenderer instance and manages the
/// lifecycle of GPU resources on the host side.
class GpuBridge {
public:
    explicit GpuBridge(RendererType type = RendererType::Software);
    ~GpuBridge();

    /// Initialize the GPU bridge and underlying renderer
    bool initialize(uint32_t width, uint32_t height);

    /// Get the renderer type
    RendererType renderer_type() const;

    // --- Resource management (called from Rust via FFI) ---

    /// Create a 2D resource
    bool resource_create_2d(uint32_t resource_id, uint32_t format,
                            uint32_t width, uint32_t height);

    /// Destroy a resource
    void resource_unref(uint32_t resource_id);

    /// Attach guest memory backing to a resource
    bool resource_attach_backing(uint32_t resource_id,
                                  const uint8_t* data, size_t size);

    /// Detach guest memory backing
    void resource_detach_backing(uint32_t resource_id);

    /// Transfer pixel data from guest to host resource
    bool transfer_to_host_2d(uint32_t resource_id,
                              uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height,
                              uint64_t offset,
                              const uint8_t* data, size_t size);

    /// Set a resource as the scanout for the display
    bool set_scanout(uint32_t scanout_id, uint32_t resource_id,
                     uint32_t x, uint32_t y,
                     uint32_t width, uint32_t height);

    /// Flush a resource region to the display
    bool resource_flush(uint32_t resource_id,
                        uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height);

    // --- 3D rendering (virgl/Venus) ---

    /// Create a 3D rendering context
    bool ctx_create(uint32_t ctx_id, const char* debug_name);

    /// Destroy a 3D rendering context
    void ctx_destroy(uint32_t ctx_id);

    /// Submit a 3D command buffer
    bool submit_3d(uint32_t ctx_id, const uint8_t* cmd_buf, size_t size);

    // --- Display output ---

    /// Get the current display framebuffer
    const FrameBuffer* get_display_framebuffer();

    /// Set callback for when the display is updated
    using DisplayUpdateCallback = std::function<void()>;
    void set_display_callback(DisplayUpdateCallback cb);

private:
    PixelFormat virtio_format_to_pixel(uint32_t format) const;

    std::unique_ptr<IRenderer> renderer_;
    RendererType type_;
    ContextHandle display_ctx_ = 0;
    uint32_t display_width_ = 0;
    uint32_t display_height_ = 0;
    DisplayUpdateCallback on_display_update_;
    std::mutex mutex_;
};

} // namespace rex::gpu
