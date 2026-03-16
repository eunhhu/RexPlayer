#include "gpu_bridge.h"
#include <cstdio>

namespace rex::gpu {

// Virtio GPU pixel formats (from virtio spec)
static constexpr uint32_t VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1;
static constexpr uint32_t VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2;
static constexpr uint32_t VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 3;
static constexpr uint32_t VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 4;
static constexpr uint32_t VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM = 67;
static constexpr uint32_t VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM = 68;
static constexpr uint32_t VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM = 121;
static constexpr uint32_t VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM = 134;

GpuBridge::GpuBridge(RendererType type) : type_(type) {}

GpuBridge::~GpuBridge() {
    if (renderer_) {
        renderer_->destroy();
    }
}

bool GpuBridge::initialize(uint32_t width, uint32_t height) {
    std::lock_guard lock(mutex_);

    display_width_ = width;
    display_height_ = height;

    renderer_ = create_renderer(type_);
    auto result = renderer_->initialize();
    if (!result) {
        fprintf(stderr, "GPU: Failed to initialize %s renderer\n",
                renderer_type_str(type_));
        return false;
    }

    // Create the primary display context
    auto ctx_result = renderer_->create_context();
    if (!ctx_result) return false;
    display_ctx_ = *ctx_result;

    return true;
}

RendererType GpuBridge::renderer_type() const {
    return type_;
}

PixelFormat GpuBridge::virtio_format_to_pixel(uint32_t format) const {
    switch (format) {
        case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
            return PixelFormat::BGRA8888;
        case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
            return PixelFormat::RGBA8888;
        case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
        case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
            return PixelFormat::XRGB8888;
        default:
            return PixelFormat::BGRA8888;
    }
}

bool GpuBridge::resource_create_2d(uint32_t resource_id, uint32_t format,
                                    uint32_t width, uint32_t height) {
    std::lock_guard lock(mutex_);
    if (!renderer_) return false;

    auto* sw = dynamic_cast<SoftwareRenderer*>(renderer_.get());
    if (sw) {
        return sw->create_resource(resource_id, width, height,
                                   virtio_format_to_pixel(format));
    }
    return false;
}

void GpuBridge::resource_unref(uint32_t resource_id) {
    std::lock_guard lock(mutex_);
    if (!renderer_) return;

    auto* sw = dynamic_cast<SoftwareRenderer*>(renderer_.get());
    if (sw) {
        sw->destroy_resource(resource_id);
    }
}

bool GpuBridge::resource_attach_backing(uint32_t resource_id,
                                         const uint8_t* data, size_t size) {
    std::lock_guard lock(mutex_);
    if (!renderer_) return false;

    auto* sw = dynamic_cast<SoftwareRenderer*>(renderer_.get());
    if (sw) {
        return sw->attach_backing(resource_id, data, size);
    }
    return false;
}

void GpuBridge::resource_detach_backing(uint32_t /*resource_id*/) {
    // Detach is a no-op for software renderer — data stays in resource
}

bool GpuBridge::transfer_to_host_2d(uint32_t resource_id,
                                     uint32_t x, uint32_t y,
                                     uint32_t width, uint32_t height,
                                     uint64_t /*offset*/,
                                     const uint8_t* data, size_t size) {
    std::lock_guard lock(mutex_);
    if (!renderer_) return false;

    auto* sw = dynamic_cast<SoftwareRenderer*>(renderer_.get());
    if (sw) {
        return sw->transfer_to_host(resource_id, x, y, width, height, data, size);
    }
    return false;
}

bool GpuBridge::set_scanout(uint32_t /*scanout_id*/, uint32_t resource_id,
                             uint32_t x, uint32_t y,
                             uint32_t width, uint32_t height) {
    std::lock_guard lock(mutex_);
    if (!renderer_) return false;

    auto* sw = dynamic_cast<SoftwareRenderer*>(renderer_.get());
    if (sw) {
        return sw->set_scanout(display_ctx_, resource_id, x, y, width, height);
    }
    return false;
}

bool GpuBridge::resource_flush(uint32_t resource_id,
                                uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height) {
    std::lock_guard lock(mutex_);
    if (!renderer_) return false;

    auto* sw = dynamic_cast<SoftwareRenderer*>(renderer_.get());
    if (sw) {
        bool ok = sw->flush_resource(resource_id, x, y, width, height);
        if (ok && on_display_update_) {
            on_display_update_();
        }
        return ok;
    }
    return false;
}

bool GpuBridge::ctx_create(uint32_t /*ctx_id*/, const char* /*debug_name*/) {
    // 3D context — requires virgl/Venus renderer
    if (type_ == RendererType::Software) return false;
    return false; // TODO: implement with virglrenderer
}

void GpuBridge::ctx_destroy(uint32_t /*ctx_id*/) {
    // TODO: virglrenderer ctx destroy
}

bool GpuBridge::submit_3d(uint32_t ctx_id, const uint8_t* cmd_buf, size_t size) {
    std::lock_guard lock(mutex_);
    if (!renderer_) return false;

    CommandBuffer cmds{};
    cmds.data = cmd_buf;
    cmds.size = size;
    cmds.ctx = ctx_id;

    auto result = renderer_->submit_commands(cmds);
    return result.has_value();
}

const FrameBuffer* GpuBridge::get_display_framebuffer() {
    std::lock_guard lock(mutex_);
    if (!renderer_) return nullptr;

    auto result = renderer_->get_framebuffer(display_ctx_);
    if (result) return *result;
    return nullptr;
}

void GpuBridge::set_display_callback(DisplayUpdateCallback cb) {
    std::lock_guard lock(mutex_);
    on_display_update_ = std::move(cb);
}

} // namespace rex::gpu
