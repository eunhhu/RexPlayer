#pragma once

#include "renderer.h"

namespace rex::gpu {

/// virglrenderer-based OpenGL renderer
///
/// Translates virtio-gpu 3D commands into host OpenGL calls via virglrenderer.
/// Used for OpenGL ES acceleration on all platforms.
class VirglRenderer : public IRenderer {
public:
    VirglRenderer();
    ~VirglRenderer() override;

    RendererType type() const override { return RendererType::Virgl; }

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
    // virglrenderer context will be stored here when linked
};

} // namespace rex::gpu
