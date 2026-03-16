#include "virgl_renderer.h"
#include <cstdio>
#include <cstring>
#include <filesystem>

// When virglrenderer is linked, uncomment:
// #include <virglrenderer.h>
// #define HAS_VIRGL 1

namespace rex::gpu {

VirglRenderer::VirglRenderer() = default;

VirglRenderer::~VirglRenderer() {
    if (initialized_) destroy();
}

RendererResult<void> VirglRenderer::initialize() {
    std::lock_guard lock(mutex_);

#ifdef HAS_VIRGL
    // Set up virglrenderer callbacks
    struct virgl_renderer_callbacks cbs = {};
    cbs.version = 1;
    cbs.write_fence = on_write_fence;
    cbs.create_gl_context = on_create_gl_context;
    cbs.destroy_gl_context = on_destroy_gl_context;
    cbs.make_current = on_make_current;

    int flags = 0; // VIRGL_RENDERER_USE_EGL
    int ret = virgl_renderer_init(this, flags, &cbs);
    if (ret != 0) {
        fprintf(stderr, "VirglRenderer: virgl_renderer_init failed: %d\n", ret);
        return std::unexpected(RendererError::InitFailed);
    }

    initialized_ = true;
#else
    fprintf(stderr, "VirglRenderer: virglrenderer not linked, "
                    "falling back to stub mode\n");
    initialized_ = true; // Stub mode for development
#endif

    // Initialize shader cache
    auto cache_dir = std::filesystem::temp_directory_path() / "rexplayer" / "shader_cache";
    shader_cache_ = std::make_unique<ShaderCache>(cache_dir);
    shader_cache_->load_index();

    return {};
}

RendererResult<ContextHandle> VirglRenderer::create_context() {
    std::lock_guard lock(mutex_);
    if (!initialized_) return std::unexpected(RendererError::InitFailed);

    ContextHandle handle = next_ctx_++;

#ifdef HAS_VIRGL
    char name[64];
    snprintf(name, sizeof(name), "rex_ctx_%u", handle);
    int ret = virgl_renderer_context_create(handle, strlen(name), name);
    if (ret != 0) {
        return std::unexpected(RendererError::ContextCreationFailed);
    }
#endif

    contexts_.insert(handle);
    return handle;
}

RendererResult<void> VirglRenderer::destroy_context(ContextHandle ctx) {
    std::lock_guard lock(mutex_);

#ifdef HAS_VIRGL
    virgl_renderer_context_destroy(ctx);
#endif

    contexts_.erase(ctx);
    return {};
}

RendererResult<void> VirglRenderer::submit_commands(const CommandBuffer& cmds) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return std::unexpected(RendererError::InitFailed);

    if (!cmds.data || cmds.size == 0) return {};

#ifdef HAS_VIRGL
    int ret = virgl_renderer_submit_cmd(
        const_cast<void*>(static_cast<const void*>(cmds.data)),
        cmds.ctx,
        static_cast<int>(cmds.size / 4) // virgl commands are in dwords
    );
    if (ret != 0) {
        return std::unexpected(RendererError::SubmitFailed);
    }
#else
    // Stub: log command submission
    (void)cmds;
#endif

    return {};
}

RendererResult<const FrameBuffer*> VirglRenderer::get_framebuffer(ContextHandle ctx) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return std::unexpected(RendererError::InitFailed);
    if (!contexts_.count(ctx)) {
        return std::unexpected(RendererError::ContextCreationFailed);
    }

#ifdef HAS_VIRGL
    // Read back the scanout resource
    // In a real implementation, we'd use virgl_renderer_get_scanout_texture()
    // and read the texture data back via glReadPixels or similar
    uint32_t width = 0, height = 0;
    uint32_t stride = 0;
    virgl_renderer_resource_get_scanout_info(0, &width, &height, &stride, nullptr);

    if (width > 0 && height > 0) {
        scanout_data_.resize(stride * height);
        // Read pixel data... (platform-specific GL readback)
        scanout_fb_.width = width;
        scanout_fb_.height = height;
        scanout_fb_.stride = stride;
        scanout_fb_.format = PixelFormat::BGRA8888;
        scanout_fb_.data = scanout_data_.data();
        return &scanout_fb_;
    }
#else
    (void)ctx;
#endif

    return std::unexpected(RendererError::InvalidFramebuffer);
}

void VirglRenderer::destroy() {
    std::lock_guard lock(mutex_);

#ifdef HAS_VIRGL
    // Destroy all contexts
    for (auto ctx : contexts_) {
        virgl_renderer_context_destroy(ctx);
    }
    virgl_renderer_cleanup(this);
#endif

    contexts_.clear();
    scanout_data_.clear();
    scanout_fb_ = {};
    initialized_ = false;
}

bool VirglRenderer::create_resource(uint32_t res_id, uint32_t target,
                                     uint32_t format, uint32_t width,
                                     uint32_t height, uint32_t depth) {
    std::lock_guard lock(mutex_);

#ifdef HAS_VIRGL
    struct virgl_renderer_resource_create_args args = {};
    args.handle = res_id;
    args.target = target;
    args.format = format;
    args.width = width;
    args.height = height;
    args.depth = depth;
    args.array_size = 1;
    args.last_level = 0;
    args.nr_samples = 0;
    args.bind = 0;

    int ret = virgl_renderer_resource_create(&args, nullptr, 0);
    return ret == 0;
#else
    (void)res_id; (void)target; (void)format;
    (void)width; (void)height; (void)depth;
    return true; // Stub
#endif
}

void VirglRenderer::destroy_resource(uint32_t res_id) {
#ifdef HAS_VIRGL
    virgl_renderer_resource_unref(res_id);
#else
    (void)res_id;
#endif
}

bool VirglRenderer::attach_resource_backing(uint32_t res_id,
                                              const uint8_t* data, size_t size) {
#ifdef HAS_VIRGL
    struct iovec iov = {};
    iov.iov_base = const_cast<void*>(static_cast<const void*>(data));
    iov.iov_len = size;
    int ret = virgl_renderer_resource_attach_iov(res_id, &iov, 1);
    return ret == 0;
#else
    (void)res_id; (void)data; (void)size;
    return true;
#endif
}

bool VirglRenderer::transfer_3d(uint32_t ctx_id, uint32_t res_id,
                                 uint32_t x, uint32_t y, uint32_t z,
                                 uint32_t w, uint32_t h, uint32_t d,
                                 uint64_t offset) {
#ifdef HAS_VIRGL
    struct virgl_box box = {};
    box.x = x; box.y = y; box.z = z;
    box.w = w; box.h = h; box.d = d;
    int ret = virgl_renderer_transfer_write_iov(
        res_id, ctx_id, 0 /* level */, 0 /* stride */,
        0 /* layer_stride */, &box, offset, nullptr, 0);
    return ret == 0;
#else
    (void)ctx_id; (void)res_id;
    (void)x; (void)y; (void)z;
    (void)w; (void)h; (void)d;
    (void)offset;
    return true;
#endif
}

void VirglRenderer::ctx_attach_resource(uint32_t ctx_id, uint32_t res_id) {
#ifdef HAS_VIRGL
    virgl_renderer_ctx_attach_resource(ctx_id, res_id);
#else
    (void)ctx_id; (void)res_id;
#endif
}

void VirglRenderer::ctx_detach_resource(uint32_t ctx_id, uint32_t res_id) {
#ifdef HAS_VIRGL
    virgl_renderer_ctx_detach_resource(ctx_id, res_id);
#else
    (void)ctx_id; (void)res_id;
#endif
}

// --- virglrenderer callbacks ---

void VirglRenderer::on_write_fence(void* cookie, uint32_t fence) {
    auto* self = static_cast<VirglRenderer*>(cookie);
    self->last_fence_ = fence;
}

int VirglRenderer::on_create_gl_context(void* /*cookie*/, int /*scanout_idx*/,
                                          void* /*create_info*/) {
    // Platform-specific EGL/WGL/CGL context creation
    // Return a context ID > 0 on success
    return 1;
}

void VirglRenderer::on_destroy_gl_context(void* /*cookie*/, int /*ctx*/) {
    // Platform-specific context destruction
}

int VirglRenderer::on_make_current(void* /*cookie*/, int /*scanout_idx*/,
                                     int /*ctx*/) {
    // Platform-specific make current
    return 0; // success
}

} // namespace rex::gpu
