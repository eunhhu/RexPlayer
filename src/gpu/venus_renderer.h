#pragma once

#include "renderer.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rex::gpu {

/// Venus (Mesa) Vulkan renderer
///
/// Translates virtio-gpu Vulkan commands into host Vulkan calls via
/// the Venus render server protocol. On macOS, uses MoltenVK as the
/// Vulkan implementation over Metal.
///
/// Protocol flow:
/// 1. Guest Mesa Venus driver encodes Vulkan commands
/// 2. virtio-gpu sends encoded command stream via SUBMIT_3D
/// 3. Venus renderer decodes and replays on host Vulkan instance
/// 4. Rendered output available via scanout resource
class VenusRenderer : public IRenderer {
public:
    VenusRenderer();
    ~VenusRenderer() override;

    RendererType type() const override { return RendererType::Venus; }
    std::string name() const override { return "Venus (Vulkan)"; }

    RendererResult<void> initialize() override;
    RendererResult<ContextHandle> create_context() override;
    RendererResult<void> destroy_context(ContextHandle ctx) override;
    RendererResult<void> submit_commands(const CommandBuffer& cmds) override;
    RendererResult<const FrameBuffer*> get_framebuffer(ContextHandle ctx) override;
    void destroy() override;

    /// Get the Vulkan API version supported
    uint32_t vulkan_api_version() const { return vulkan_version_; }

    /// Check if a Vulkan extension is supported
    bool has_extension(const char* name) const;

private:
    bool initialized_ = false;
    std::mutex mutex_;
    uint32_t next_ctx_ = 1;
    uint32_t vulkan_version_ = 0; // VK_API_VERSION

    std::unordered_set<ContextHandle> contexts_;
    std::vector<std::string> extensions_;

    // Scanout
    FrameBuffer scanout_fb_;
    std::vector<uint8_t> scanout_data_;
};

} // namespace rex::gpu
