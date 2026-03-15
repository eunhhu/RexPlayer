#include "virgl_renderer.h"
#include <cstdio>

// TODO: #include <virglrenderer.h> when linked

namespace rex::gpu {

VirglRenderer::VirglRenderer() = default;

VirglRenderer::~VirglRenderer() {
    if (initialized_) {
        destroy();
    }
}

RendererResult<void> VirglRenderer::initialize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;

    // TODO: virgl_renderer_init() with callbacks
    // - write_fence callback
    // - create_gl_context callback
    // - make_current callback
    //
    // For now, return NotSupported until virglrenderer is linked
    fprintf(stderr, "VirglRenderer: virglrenderer library not linked yet\n");
    return std::unexpected(RendererError::NotSupported);
}

RendererResult<ContextHandle> VirglRenderer::create_context(uint32_t ctx_id) {
    if (!initialized_) return std::unexpected(RendererError::NotInitialized);

    // TODO: virgl_renderer_context_create()
    return static_cast<ContextHandle>(ctx_id);
}

void VirglRenderer::destroy_context(ContextHandle /*handle*/) {
    // TODO: virgl_renderer_context_destroy()
}

RendererResult<void> VirglRenderer::submit_commands(
    ContextHandle /*ctx*/, const CommandBuffer& /*cmds*/)
{
    if (!initialized_) return std::unexpected(RendererError::NotInitialized);
    // TODO: virgl_renderer_submit_cmd()
    return {};
}

RendererResult<FrameBuffer> VirglRenderer::get_framebuffer() {
    if (!initialized_) return std::unexpected(RendererError::NotInitialized);

    // TODO: read back from virglrenderer's scanout resource
    FrameBuffer fb{};
    fb.width = width_;
    fb.height = height_;
    fb.format = PixelFormat::BGRA8888;
    fb.stride = width_ * 4;
    fb.data = nullptr; // Will point to the rendered output
    return fb;
}

void VirglRenderer::destroy() {
    if (initialized_) {
        // TODO: virgl_renderer_cleanup()
        initialized_ = false;
    }
}

} // namespace rex::gpu
