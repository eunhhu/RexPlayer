#include "software_renderer.h"
#include <algorithm>
#include <cstring>

namespace rex::gpu {

SoftwareRenderer::SoftwareRenderer() = default;

SoftwareRenderer::~SoftwareRenderer() {
    if (initialized_) destroy();
}

RendererResult<void> SoftwareRenderer::initialize() {
    std::lock_guard lock(mutex_);
    initialized_ = true;
    return {};
}

RendererResult<ContextHandle> SoftwareRenderer::create_context() {
    std::lock_guard lock(mutex_);
    if (!initialized_) return std::unexpected(RendererError::InitFailed);

    ContextHandle handle = next_ctx_++;
    Context ctx{};
    ctx.handle = handle;
    contexts_[handle] = std::move(ctx);
    return handle;
}

RendererResult<void> SoftwareRenderer::destroy_context(ContextHandle ctx) {
    std::lock_guard lock(mutex_);
    contexts_.erase(ctx);
    return {};
}

RendererResult<void> SoftwareRenderer::submit_commands(const CommandBuffer& /*cmds*/) {
    // Software renderer doesn't process 3D commands
    // 2D operations go through the resource management APIs
    return {};
}

RendererResult<const FrameBuffer*> SoftwareRenderer::get_framebuffer(ContextHandle ctx) {
    std::lock_guard lock(mutex_);

    auto it = contexts_.find(ctx);
    if (it == contexts_.end()) {
        return std::unexpected(RendererError::ContextCreationFailed);
    }

    auto& context = it->second;
    if (context.scanout_resource_id == 0) {
        return std::unexpected(RendererError::InvalidFramebuffer);
    }

    auto res_it = resources_.find(context.scanout_resource_id);
    if (res_it == resources_.end()) {
        return std::unexpected(RendererError::InvalidFramebuffer);
    }

    auto& res = res_it->second;
    context.fb.width = res.width;
    context.fb.height = res.height;
    context.fb.format = res.format;
    context.fb.stride = res.width * bytes_per_pixel(res.format);
    context.fb.data = res.data.data();

    return &context.fb;
}

void SoftwareRenderer::destroy() {
    std::lock_guard lock(mutex_);
    resources_.clear();
    contexts_.clear();
    initialized_ = false;
}

bool SoftwareRenderer::create_resource(uint32_t resource_id, uint32_t width,
                                        uint32_t height, PixelFormat format) {
    std::lock_guard lock(mutex_);

    if (resources_.count(resource_id)) return false;

    Resource res{};
    res.id = resource_id;
    res.width = width;
    res.height = height;
    res.format = format;

    size_t size = static_cast<size_t>(width) * height * bytes_per_pixel(format);
    res.data.resize(size, 0);

    resources_[resource_id] = std::move(res);
    return true;
}

void SoftwareRenderer::destroy_resource(uint32_t resource_id) {
    std::lock_guard lock(mutex_);

    // Remove from any scanouts
    for (auto& [_, ctx] : contexts_) {
        if (ctx.scanout_resource_id == resource_id) {
            ctx.scanout_resource_id = 0;
            ctx.fb = {};
        }
    }

    resources_.erase(resource_id);
}

bool SoftwareRenderer::attach_backing(uint32_t resource_id, const uint8_t* data,
                                       size_t size) {
    std::lock_guard lock(mutex_);

    auto it = resources_.find(resource_id);
    if (it == resources_.end()) return false;

    auto& res = it->second;
    size_t expected = static_cast<size_t>(res.width) * res.height * bytes_per_pixel(res.format);
    size_t copy_size = std::min(size, expected);

    if (data && copy_size > 0) {
        std::memcpy(res.data.data(), data, copy_size);
    }
    res.has_backing = true;
    return true;
}

bool SoftwareRenderer::transfer_to_host(uint32_t resource_id, uint32_t x, uint32_t y,
                                         uint32_t width, uint32_t height,
                                         const uint8_t* data, size_t size) {
    std::lock_guard lock(mutex_);

    auto it = resources_.find(resource_id);
    if (it == resources_.end()) return false;

    auto& res = it->second;
    uint32_t bpp = bytes_per_pixel(res.format);
    uint32_t res_stride = res.width * bpp;
    uint32_t src_stride = width * bpp;

    // Bounds check
    if (x + width > res.width || y + height > res.height) return false;
    if (size < static_cast<size_t>(src_stride) * height) return false;

    // Copy scanlines
    for (uint32_t row = 0; row < height; ++row) {
        size_t dst_offset = static_cast<size_t>(y + row) * res_stride + x * bpp;
        size_t src_offset = static_cast<size_t>(row) * src_stride;
        std::memcpy(res.data.data() + dst_offset, data + src_offset, src_stride);
    }

    return true;
}

bool SoftwareRenderer::set_scanout(ContextHandle ctx, uint32_t resource_id,
                                    uint32_t /*x*/, uint32_t /*y*/,
                                    uint32_t /*width*/, uint32_t /*height*/) {
    std::lock_guard lock(mutex_);

    auto ctx_it = contexts_.find(ctx);
    if (ctx_it == contexts_.end()) return false;

    if (resource_id != 0 && !resources_.count(resource_id)) return false;

    ctx_it->second.scanout_resource_id = resource_id;
    return true;
}

bool SoftwareRenderer::flush_resource(uint32_t resource_id,
                                       uint32_t /*x*/, uint32_t /*y*/,
                                       uint32_t /*width*/, uint32_t /*height*/) {
    std::lock_guard lock(mutex_);
    // In software mode, the resource data is already up-to-date
    // The flush is a no-op — the display reads directly from resource data
    return resources_.count(resource_id) > 0;
}

// Factory function implementation
std::unique_ptr<IRenderer> create_renderer(RendererType type) {
    switch (type) {
        case RendererType::Software:
            return std::make_unique<SoftwareRenderer>();
        case RendererType::Virgl:
            // TODO: return std::make_unique<VirglRenderer>();
            return std::make_unique<SoftwareRenderer>(); // fallback
        case RendererType::Venus:
            // TODO: return std::make_unique<VenusRenderer>();
            return std::make_unique<SoftwareRenderer>(); // fallback
    }
    return std::make_unique<SoftwareRenderer>();
}

} // namespace rex::gpu
