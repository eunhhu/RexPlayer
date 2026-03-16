#include "venus_renderer.h"
#include <cstdio>
#include <cstring>

// When Venus/Vulkan is linked:
// #include <vulkan/vulkan.h>
// #define HAS_VENUS 1

namespace rex::gpu {

VenusRenderer::VenusRenderer() = default;

VenusRenderer::~VenusRenderer() {
    if (initialized_) destroy();
}

RendererResult<void> VenusRenderer::initialize() {
    std::lock_guard lock(mutex_);

#ifdef HAS_VENUS
    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "RexPlayer";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    #ifdef __APPLE__
    create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    #endif

    VkInstance instance;
    if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
        return std::unexpected(RendererError::InitFailed);
    }
    vulkan_version_ = VK_API_VERSION_1_3;
    initialized_ = true;
#else
    fprintf(stderr, "VenusRenderer: Vulkan not linked, stub mode\n");
    vulkan_version_ = 0;
    initialized_ = true;
#endif

    return {};
}

RendererResult<ContextHandle> VenusRenderer::create_context() {
    std::lock_guard lock(mutex_);
    if (!initialized_) return std::unexpected(RendererError::InitFailed);

    ContextHandle handle = next_ctx_++;
    contexts_.insert(handle);
    return handle;
}

RendererResult<void> VenusRenderer::destroy_context(ContextHandle ctx) {
    std::lock_guard lock(mutex_);
    contexts_.erase(ctx);
    return {};
}

RendererResult<void> VenusRenderer::submit_commands(const CommandBuffer& cmds) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return std::unexpected(RendererError::InitFailed);
    if (!cmds.data || cmds.size == 0) return {};

#ifdef HAS_VENUS
    // Decode Venus protocol and replay on host Vulkan
#else
    (void)cmds;
#endif

    return {};
}

RendererResult<const FrameBuffer*> VenusRenderer::get_framebuffer(ContextHandle ctx) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return std::unexpected(RendererError::InitFailed);
    if (!contexts_.count(ctx)) {
        return std::unexpected(RendererError::ContextCreationFailed);
    }

    // Stub: no framebuffer until Vulkan readback is implemented
    (void)ctx;
    return std::unexpected(RendererError::InvalidFramebuffer);
}

void VenusRenderer::destroy() {
    std::lock_guard lock(mutex_);
    contexts_.clear();
    scanout_data_.clear();
    scanout_fb_ = {};
    initialized_ = false;
}

bool VenusRenderer::has_extension(const char* ext_name) const {
    for (const auto& ext : extensions_) {
        if (ext == ext_name) return true;
    }
    return false;
}

} // namespace rex::gpu
