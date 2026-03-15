#pragma once

#include "renderer.h"

namespace rex::gpu {

/// Venus (Mesa) Vulkan renderer
///
/// Translates virtio-gpu Vulkan commands into host Vulkan calls via
/// the Venus render server protocol. Used for Vulkan acceleration.
class VenusRenderer : public IRenderer {
public:
    VenusRenderer();
    ~VenusRenderer() override;

    RendererType type() const override { return RendererType::Venus; }

    RendererResult<void> initialize(uint32_t width, uint32_t height) override;
    RendererResult<ContextHandle> create_context(uint32_t ctx_id) override;
    void destroy_context(ContextHandle handle) override;
    RendererResult<void> submit_commands(ContextHandle ctx,
                                         const CommandBuffer& cmds) override;
    RendererResult<FrameBuffer> get_framebuffer() override;
    void destroy() override;

private:
    bool initialized_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace rex::gpu
