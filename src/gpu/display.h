#pragma once

#include "renderer.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <functional>

namespace rex::gpu {

/// Display manages a double-buffered framebuffer for presenting
/// rendered frames to the host window.
///
/// The back buffer receives new frames from the renderer; calling
/// present() atomically swaps front and back so the GUI thread
/// can read the front buffer without tearing.
class Display {
public:
    /// Callback invoked after present() swaps the buffers.
    /// The argument is a read-only view of the new front buffer.
    using PresentCallback = std::function<void(const FrameBuffer&)>;

    Display();
    ~Display();

    // Non-copyable, movable
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;
    Display(Display&&) noexcept;
    Display& operator=(Display&&) noexcept;

    /// Resize the display (re-allocates both buffers).
    /// Any in-flight frame data is discarded.
    /// @param width   Framebuffer width in pixels (must be > 0)
    /// @param height  Framebuffer height in pixels (must be > 0)
    /// @param format  Pixel format (default BGRA8888)
    /// @return true on success, false if parameters are invalid
    bool resize(uint32_t width, uint32_t height,
                PixelFormat format = PixelFormat::BGRA8888);

    /// Get a mutable reference to the back buffer for writing new frame data.
    /// The caller should fill this buffer, then call present().
    FrameBuffer& back_buffer();

    /// Get a read-only reference to the front buffer (the last presented frame).
    /// Safe to call from the GUI/render thread while the back buffer is being written.
    const FrameBuffer& front_buffer() const;

    /// Copy a complete frame into the back buffer from an external source.
    /// @param src   Pointer to source pixel data
    /// @param size  Number of bytes to copy (must match back buffer size)
    /// @return true if the copy succeeded
    bool write_frame(const uint8_t* src, size_t size);

    /// Swap front and back buffers (present the latest frame).
    /// This is an atomic pointer swap protected by a mutex so the GUI
    /// thread can safely read the front buffer at any time.
    void present();

    /// Register a callback that fires after each present().
    void set_present_callback(PresentCallback cb);

    /// Current display width (0 if not yet resized)
    [[nodiscard]] uint32_t width() const;

    /// Current display height (0 if not yet resized)
    [[nodiscard]] uint32_t height() const;

    /// Whether the display has been allocated
    [[nodiscard]] bool is_ready() const;

    /// Number of frames presented since creation / last resize
    [[nodiscard]] uint64_t frame_count() const;

private:
    void allocate_buffers(uint32_t width, uint32_t height,
                          uint32_t stride, PixelFormat format);
    void release_buffers();

    struct BufferState {
        FrameBuffer fb;
        std::vector<uint8_t> storage; // owning pixel memory
    };

    BufferState buffers_[2];       // double-buffer pair
    int front_index_ = 0;         // index of the current front buffer
    mutable std::mutex swap_mutex_;

    PresentCallback present_cb_;
    uint64_t frame_count_ = 0;
};

} // namespace rex::gpu
