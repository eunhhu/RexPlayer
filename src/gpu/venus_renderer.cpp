#include "venus_renderer.h"
#include <cstdio>

namespace rex::gpu {

VenusRenderer::VenusRenderer() = default;

VenusRenderer::~VenusRenderer() {
    if (initialized_) destroy();
}

RendererResult<void> VenusRenderer::initialize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;

    // TODO: Initialize Venus render server
    // Requires:
    // - Host Vulkan instance creation
    // - Venus protocol setup
    // - Ring buffer for command submission
    fprintf(stderr, "VenusRenderer: Venus render server not linked yet\n");
    return std::unexpected(RendererError::NotSupported);
}

RendererResult<ContextHandle> VenusRenderer::create_context(uint32_t ctx_id) {
    if (!initialized_) return std::unexpected(RendererError::NotInitialized);
    return static_cast<ContextHandle>(ctx_id);
}

void VenusRenderer::destroy_context(ContextHandle /*handle*/) {}

RendererResult<void> VenusRenderer::submit_commands(
    ContextHandle /*ctx*/, const CommandBuffer& /*cmds*/)
{
    if (!initialized_) return std::unexpected(RendererError::NotInitialized);
    return {};
}

RendererResult<FrameBuffer> VenusRenderer::get_framebuffer() {
    if (!initialized_) return std::unexpected(RendererError::NotInitialized);

    FrameBuffer fb{};
    fb.width = width_;
    fb.height = height_;
    fb.format = PixelFormat::BGRA8888;
    fb.stride = width_ * 4;
    fb.data = nullptr;
    return fb;
}

void VenusRenderer::destroy() {
    if (initialized_) {
        initialized_ = false;
    }
}

} // namespace rex::gpu
