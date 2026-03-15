#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>
#include <expected>
#include <string>

namespace rex::gpu {

/// Pixel format for framebuffer data
enum class PixelFormat : uint32_t {
    BGRA8888,   // 32-bit BGRA (native for most displays)
    RGBA8888,   // 32-bit RGBA
    RGB565,     // 16-bit RGB (Android default)
    XRGB8888,   // 32-bit xRGB (alpha ignored)
};

/// Returns bytes per pixel for a given format
constexpr uint32_t bytes_per_pixel(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::BGRA8888: return 4;
        case PixelFormat::RGBA8888: return 4;
        case PixelFormat::RGB565:   return 2;
        case PixelFormat::XRGB8888: return 4;
    }
    return 4;
}

/// Framebuffer descriptor — holds dimensions and a pointer to pixel data
struct FrameBuffer {
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t stride = 0;       // bytes per row (may include padding)
    PixelFormat format = PixelFormat::BGRA8888;
    uint8_t* data   = nullptr; // owned externally; renderer manages lifetime

    /// Total size of the pixel data in bytes
    [[nodiscard]] size_t size_bytes() const {
        return static_cast<size_t>(stride) * height;
    }

    /// Whether this framebuffer has been allocated
    [[nodiscard]] bool is_valid() const {
        return data != nullptr && width > 0 && height > 0;
    }
};

/// GPU renderer backend type
enum class RendererType : uint32_t {
    Software,   // CPU-based scanline renderer (fallback)
    Virgl,      // VirGL 3D (virtio-gpu, OpenGL passthrough)
    Venus,      // Venus (virtio-gpu, Vulkan passthrough)
};

/// Returns a human-readable name for the renderer type
inline const char* renderer_type_str(RendererType type) {
    switch (type) {
        case RendererType::Software: return "Software";
        case RendererType::Virgl:    return "Virgl";
        case RendererType::Venus:    return "Venus";
    }
    return "Unknown";
}

/// Error codes for renderer operations
enum class RendererError : uint32_t {
    Ok = 0,
    NotSupported,
    InitFailed,
    ContextCreationFailed,
    SubmitFailed,
    InvalidFramebuffer,
    OutOfMemory,
    DeviceLost,
};

template <typename T>
using RendererResult = std::expected<T, RendererError>;

/// Opaque handle to a GPU rendering context (one per guest surface)
using ContextHandle = uint32_t;

/// Command buffer submitted by the guest GPU driver
struct CommandBuffer {
    const uint8_t* data = nullptr;
    size_t size = 0;
    ContextHandle ctx = 0;
};

/// Interface for GPU renderers
///
/// Each backend (Software, Virgl, Venus) implements this interface.
/// The renderer is responsible for processing guest GPU commands and
/// producing framebuffers that can be displayed to the host.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    /// Get the renderer backend type
    virtual RendererType type() const = 0;

    /// Get a human-readable name
    virtual std::string name() const = 0;

    /// Initialize the renderer (create host GPU resources, compile shaders, etc.)
    /// Must be called before any other method.
    virtual RendererResult<void> initialize() = 0;

    /// Create a new rendering context for a guest surface
    /// Returns an opaque handle used for subsequent operations.
    virtual RendererResult<ContextHandle> create_context() = 0;

    /// Destroy a previously created rendering context
    virtual RendererResult<void> destroy_context(ContextHandle ctx) = 0;

    /// Submit a command buffer from the guest GPU driver for execution
    virtual RendererResult<void> submit_commands(const CommandBuffer& cmds) = 0;

    /// Get the current framebuffer for the given context
    /// The returned framebuffer is valid until the next submit_commands call
    /// or until the context is destroyed.
    virtual RendererResult<const FrameBuffer*> get_framebuffer(ContextHandle ctx) = 0;

    /// Tear down the renderer and release all host GPU resources
    virtual void destroy() = 0;
};

/// Factory: create a renderer of the given type
std::unique_ptr<IRenderer> create_renderer(RendererType type);

} // namespace rex::gpu
