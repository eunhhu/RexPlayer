#include "display.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>

namespace rex::gpu {

// ---------------------------------------------------------------------------
// Construction / destruction / move
// ---------------------------------------------------------------------------

Display::Display() = default;

Display::~Display() {
    release_buffers();
}

Display::Display(Display&& other) noexcept {
    std::lock_guard lock(other.swap_mutex_);
    buffers_[0] = std::move(other.buffers_[0]);
    buffers_[1] = std::move(other.buffers_[1]);
    front_index_ = other.front_index_;
    present_cb_ = std::move(other.present_cb_);
    frame_count_ = other.frame_count_;
    other.front_index_ = 0;
    other.frame_count_ = 0;
}

Display& Display::operator=(Display&& other) noexcept {
    if (this != &other) {
        // Lock both mutexes in consistent address order to avoid deadlock
        std::mutex* first  = &swap_mutex_;
        std::mutex* second = &other.swap_mutex_;
        if (first > second) std::swap(first, second);

        std::lock_guard lock1(*first);
        std::lock_guard lock2(*second);

        release_buffers();
        buffers_[0] = std::move(other.buffers_[0]);
        buffers_[1] = std::move(other.buffers_[1]);
        front_index_ = other.front_index_;
        present_cb_ = std::move(other.present_cb_);
        frame_count_ = other.frame_count_;
        other.front_index_ = 0;
        other.frame_count_ = 0;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Buffer management
// ---------------------------------------------------------------------------

void Display::allocate_buffers(uint32_t width, uint32_t height,
                               uint32_t stride, PixelFormat format) {
    const size_t buf_size = static_cast<size_t>(stride) * height;

    for (auto& bs : buffers_) {
        bs.storage.resize(buf_size);
        std::fill(bs.storage.begin(), bs.storage.end(), uint8_t{0});

        bs.fb.width  = width;
        bs.fb.height = height;
        bs.fb.stride = stride;
        bs.fb.format = format;
        bs.fb.data   = bs.storage.data();
    }
}

void Display::release_buffers() {
    for (auto& bs : buffers_) {
        bs.storage.clear();
        bs.storage.shrink_to_fit();
        bs.fb = FrameBuffer{};
    }
    frame_count_ = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool Display::resize(uint32_t width, uint32_t height, PixelFormat format) {
    if (width == 0 || height == 0) {
        return false;
    }

    const uint32_t bpp = bytes_per_pixel(format);
    // Align stride to 64-byte boundary for SIMD-friendly access
    const uint32_t raw_stride = width * bpp;
    const uint32_t stride = (raw_stride + 63u) & ~63u;

    std::lock_guard lock(swap_mutex_);
    release_buffers();
    allocate_buffers(width, height, stride, format);
    front_index_ = 0;
    return true;
}

FrameBuffer& Display::back_buffer() {
    // Back buffer is the one NOT currently being displayed
    const int back = 1 - front_index_;
    return buffers_[back].fb;
}

const FrameBuffer& Display::front_buffer() const {
    std::lock_guard lock(swap_mutex_);
    return buffers_[front_index_].fb;
}

bool Display::write_frame(const uint8_t* src, size_t size) {
    if (!src) return false;

    FrameBuffer& bb = back_buffer();
    if (!bb.is_valid()) return false;

    const size_t expected = bb.size_bytes();
    if (size != expected) return false;

    std::memcpy(bb.data, src, size);
    return true;
}

void Display::present() {
    PresentCallback cb_copy;
    {
        std::lock_guard lock(swap_mutex_);
        // Swap by flipping the index — O(1), no data copy
        front_index_ = 1 - front_index_;
        ++frame_count_;
        cb_copy = present_cb_; // copy under lock for thread safety
    }

    if (cb_copy) {
        // Invoke callback outside the lock to avoid deadlocks
        cb_copy(buffers_[front_index_].fb);
    }
}

void Display::set_present_callback(PresentCallback cb) {
    std::lock_guard lock(swap_mutex_);
    present_cb_ = std::move(cb);
}

uint32_t Display::width() const {
    std::lock_guard lock(swap_mutex_);
    return buffers_[front_index_].fb.width;
}

uint32_t Display::height() const {
    std::lock_guard lock(swap_mutex_);
    return buffers_[front_index_].fb.height;
}

bool Display::is_ready() const {
    std::lock_guard lock(swap_mutex_);
    return buffers_[0].fb.is_valid() && buffers_[1].fb.is_valid();
}

uint64_t Display::frame_count() const {
    std::lock_guard lock(swap_mutex_);
    return frame_count_;
}

} // namespace rex::gpu
