#pragma once

#include "renderer.h"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rex::gpu {

/// Software renderer — CPU-based framebuffer management
///
/// This is the fallback renderer when no GPU acceleration is available.
/// It manages 2D resources (scanout framebuffers) and performs simple
/// blit operations. Used for:
/// - Initial boot display (before GPU drivers load)
/// - SwiftShader software rendering path
/// - Testing without GPU hardware
class SoftwareRenderer : public IRenderer {
public:
    SoftwareRenderer();
    ~SoftwareRenderer() override;

    RendererType type() const override { return RendererType::Software; }
    std::string name() const override { return "Software"; }

    RendererResult<void> initialize() override;
    RendererResult<ContextHandle> create_context() override;
    RendererResult<void> destroy_context(ContextHandle ctx) override;
    RendererResult<void> submit_commands(const CommandBuffer& cmds) override;
    RendererResult<const FrameBuffer*> get_framebuffer(ContextHandle ctx) override;
    void destroy() override;

    /// Create a 2D resource (used by virtio-gpu RESOURCE_CREATE_2D)
    bool create_resource(uint32_t resource_id, uint32_t width, uint32_t height,
                         PixelFormat format);

    /// Destroy a resource
    void destroy_resource(uint32_t resource_id);

    /// Attach backing memory to a resource
    bool attach_backing(uint32_t resource_id, const uint8_t* data, size_t size);

    /// Transfer data from guest to resource
    bool transfer_to_host(uint32_t resource_id, uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height,
                          const uint8_t* data, size_t size);

    /// Set a resource as the scanout source for a context
    bool set_scanout(ContextHandle ctx, uint32_t resource_id,
                     uint32_t x, uint32_t y, uint32_t width, uint32_t height);

    /// Flush a region of a resource to the display
    bool flush_resource(uint32_t resource_id, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height);

private:
    struct Resource {
        uint32_t id;
        uint32_t width;
        uint32_t height;
        PixelFormat format;
        std::vector<uint8_t> data;
        bool has_backing = false;
    };

    struct Context {
        ContextHandle handle;
        uint32_t scanout_resource_id = 0;
        FrameBuffer fb;
    };

    bool initialized_ = false;
    std::mutex mutex_;
    uint32_t next_ctx_ = 1;

    std::unordered_map<uint32_t, Resource> resources_;
    std::unordered_map<ContextHandle, Context> contexts_;
};

} // namespace rex::gpu
