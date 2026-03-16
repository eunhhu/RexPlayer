#pragma once

#include "renderer.h"
#include "shader_cache.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace rex::gpu {

/// virglrenderer-based OpenGL renderer
///
/// Translates virtio-gpu 3D commands into host OpenGL calls via virglrenderer.
/// Used for OpenGL ES acceleration on all platforms.
///
/// Lifecycle:
/// 1. initialize() — calls virgl_renderer_init() with callbacks
/// 2. create_context() — calls virgl_renderer_context_create()
/// 3. submit_commands() — calls virgl_renderer_submit_cmd()
/// 4. get_framebuffer() — reads back from virglrenderer scanout
/// 5. destroy() — calls virgl_renderer_cleanup()
class VirglRenderer : public IRenderer {
public:
    VirglRenderer();
    ~VirglRenderer() override;

    RendererType type() const override { return RendererType::Virgl; }
    std::string name() const override { return "Virgl (OpenGL)"; }

    RendererResult<void> initialize() override;
    RendererResult<ContextHandle> create_context() override;
    RendererResult<void> destroy_context(ContextHandle ctx) override;
    RendererResult<void> submit_commands(const CommandBuffer& cmds) override;
    RendererResult<const FrameBuffer*> get_framebuffer(ContextHandle ctx) override;
    void destroy() override;

    /// Create a virgl 3D resource
    bool create_resource(uint32_t res_id, uint32_t target, uint32_t format,
                         uint32_t width, uint32_t height, uint32_t depth);

    /// Destroy a virgl resource
    void destroy_resource(uint32_t res_id);

    /// Attach iov backing to a resource
    bool attach_resource_backing(uint32_t res_id,
                                  const uint8_t* data, size_t size);

    /// Transfer data from guest to virgl resource
    bool transfer_3d(uint32_t ctx_id, uint32_t res_id,
                     uint32_t x, uint32_t y, uint32_t z,
                     uint32_t w, uint32_t h, uint32_t d,
                     uint64_t offset);

    /// Attach a resource to a context
    void ctx_attach_resource(uint32_t ctx_id, uint32_t res_id);

    /// Detach a resource from a context
    void ctx_detach_resource(uint32_t ctx_id, uint32_t res_id);

    /// Get the shader cache
    ShaderCache* shader_cache() { return shader_cache_.get(); }

private:
    // virglrenderer callbacks (static, forwarded to instance)
    static void on_write_fence(void* cookie, uint32_t fence);
    static int on_create_gl_context(void* cookie, int scanout_idx,
                                     void* create_info);
    static void on_destroy_gl_context(void* cookie, int ctx);
    static int on_make_current(void* cookie, int scanout_idx, int ctx);

    bool initialized_ = false;
    std::mutex mutex_;
    uint32_t next_ctx_ = 1;

    // Active contexts
    std::unordered_set<ContextHandle> contexts_;

    // Scanout framebuffer
    FrameBuffer scanout_fb_;
    std::vector<uint8_t> scanout_data_;

    // Shader cache
    std::unique_ptr<ShaderCache> shader_cache_;

    // Fence tracking
    uint32_t last_fence_ = 0;
};

} // namespace rex::gpu
