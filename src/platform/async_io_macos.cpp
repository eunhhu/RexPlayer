#ifdef __APPLE__

#include "async_io.h"

#include <dispatch/dispatch.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace rex::platform {

// ============================================================================
// DispatchIoAsyncIo — GCD (dispatch_io) based async I/O for macOS
// ============================================================================

class DispatchIoAsyncIo : public IAsyncIo {
public:
    DispatchIoAsyncIo();
    ~DispatchIoAsyncIo() override;

    bool initialize(uint32_t queue_depth) override;
    void shutdown() override;
    bool register_handle(IoHandle handle) override;
    void unregister_handle(IoHandle handle) override;

    bool submit_read(IoHandle handle, void* buffer, size_t size,
                     int64_t offset, uint64_t user_data,
                     IoCallback callback) override;

    bool submit_write(IoHandle handle, const void* buffer, size_t size,
                      int64_t offset, uint64_t user_data,
                      IoCallback callback) override;

    uint32_t poll() override;
    uint32_t wait(std::chrono::milliseconds timeout) override;
    uint32_t pending_count() const override;

private:
    /// Per-handle dispatch I/O channel
    struct ChannelEntry {
        dispatch_io_t channel;
    };

    /// Completed I/O result queued for delivery
    struct CompletionEntry {
        IoCompletion completion;
        IoCallback callback;
    };

    /// Queue a completion for delivery during poll()/wait()
    void enqueue_completion(IoCompletion completion, IoCallback callback);

    /// Drain the completion queue, invoking callbacks
    uint32_t drain_completions();

    // GCD concurrent queue for I/O dispatch
    dispatch_queue_t io_queue_ = nullptr;

    // Registered dispatch_io channels per fd
    std::mutex channels_mutex_;
    std::unordered_map<IoHandle, ChannelEntry> channels_;

    // Completion queue (produced by GCD callbacks, consumed by poll/wait)
    std::mutex completions_mutex_;
    std::vector<CompletionEntry> completions_;

    // Semaphore for blocking wait()
    dispatch_semaphore_t completion_sem_ = nullptr;

    std::atomic<uint32_t> inflight_{0};
    uint32_t queue_depth_ = 256;
    bool initialized_ = false;
};

// ============================================================================
// Implementation
// ============================================================================

DispatchIoAsyncIo::DispatchIoAsyncIo() = default;

DispatchIoAsyncIo::~DispatchIoAsyncIo() {
    shutdown();
}

bool DispatchIoAsyncIo::initialize(uint32_t queue_depth) {
    if (initialized_) return false;

    queue_depth_ = queue_depth;

    // Create a high-priority concurrent queue for I/O
    io_queue_ = dispatch_queue_create(
        "com.rexplayer.async_io",
        dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_CONCURRENT,
            QOS_CLASS_USER_INITIATED,
            0
        )
    );
    if (!io_queue_) return false;

    completion_sem_ = dispatch_semaphore_create(0);
    if (!completion_sem_) {
        dispatch_release(io_queue_);
        io_queue_ = nullptr;
        return false;
    }

    initialized_ = true;
    return true;
}

void DispatchIoAsyncIo::shutdown() {
    if (!initialized_) return;

    // Close all registered channels
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        for (auto& [fd, entry] : channels_) {
            dispatch_io_close(entry.channel, DISPATCH_IO_STOP);
            dispatch_release(entry.channel);
        }
        channels_.clear();
    }

    // Drain remaining completions
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        completions_.clear();
    }

    if (completion_sem_) {
        dispatch_release(completion_sem_);
        completion_sem_ = nullptr;
    }

    if (io_queue_) {
        dispatch_release(io_queue_);
        io_queue_ = nullptr;
    }

    inflight_.store(0);
    initialized_ = false;
}

bool DispatchIoAsyncIo::register_handle(IoHandle handle) {
    if (!initialized_) return false;

    std::lock_guard<std::mutex> lock(channels_mutex_);
    if (channels_.count(handle)) return true; // Already registered

    // Create a random-access dispatch_io channel for this fd
    dispatch_io_t channel = dispatch_io_create(
        DISPATCH_IO_RANDOM,
        handle,
        io_queue_,
        ^(int /*error*/) {
            // Cleanup handler — channel is being closed
        }
    );

    if (!channel) return false;

    // Set high/low water marks to control coalescing behavior
    dispatch_io_set_high_water(channel, SIZE_MAX); // Deliver all data at once
    dispatch_io_set_low_water(channel, 1);          // Don't wait for minimum

    channels_[handle] = ChannelEntry{channel};
    return true;
}

void DispatchIoAsyncIo::unregister_handle(IoHandle handle) {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    auto it = channels_.find(handle);
    if (it != channels_.end()) {
        dispatch_io_close(it->second.channel, DISPATCH_IO_STOP);
        dispatch_release(it->second.channel);
        channels_.erase(it);
    }
}

bool DispatchIoAsyncIo::submit_read(IoHandle handle, void* buffer, size_t size,
                                     int64_t offset, uint64_t user_data,
                                     IoCallback callback) {
    if (!initialized_) return false;

    dispatch_io_t channel = nullptr;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        auto it = channels_.find(handle);
        if (it == channels_.end()) return false;
        channel = it->second.channel;
    }

    inflight_.fetch_add(1, std::memory_order_relaxed);

    // Capture by value for the block
    uint64_t ud = user_data;
    IoCallback cb = std::move(callback);
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    size_t total_read = 0;

    // Use the offset; if -1 was passed, use 0 (current position not meaningful
    // for DISPATCH_IO_RANDOM channels)
    off_t file_offset = offset >= 0 ? static_cast<off_t>(offset) : 0;

    dispatch_io_read(
        channel,
        file_offset,
        size,
        io_queue_,
        ^(bool done, dispatch_data_t data, int error) {
            if (data) {
                // Copy delivered data into the caller's buffer
                dispatch_data_apply(data,
                    ^bool(dispatch_data_t /*region*/, size_t region_offset,
                          const void* region_buf, size_t region_size) {
                        (void)region_offset;
                        if (total_read + region_size <= size) {
                            std::memcpy(dst + total_read, region_buf, region_size);
                            total_read += region_size;
                        }
                        return true; // continue
                    }
                );
            }

            if (done) {
                IoCompletion completion{};
                if (error == 0) {
                    completion.status = IoCompletion::Status::Success;
                    completion.bytes_transferred = total_read;
                } else if (error == ECANCELED) {
                    completion.status = IoCompletion::Status::Cancelled;
                    completion.error_code = error;
                } else {
                    completion.status = IoCompletion::Status::Error;
                    completion.error_code = error;
                }
                completion.user_data = ud;

                enqueue_completion(completion, cb);
                inflight_.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    );

    return true;
}

bool DispatchIoAsyncIo::submit_write(IoHandle handle, const void* buffer, size_t size,
                                      int64_t offset, uint64_t user_data,
                                      IoCallback callback) {
    if (!initialized_) return false;

    dispatch_io_t channel = nullptr;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        auto it = channels_.find(handle);
        if (it == channels_.end()) return false;
        channel = it->second.channel;
    }

    inflight_.fetch_add(1, std::memory_order_relaxed);

    // Create dispatch_data from the source buffer
    // The buffer must remain valid for the duration of the write.
    // DISPATCH_DATA_DESTRUCTOR_DEFAULT copies the data internally — but that's
    // wasteful for large writes. We use a no-op destructor instead, relying on
    // the caller to keep the buffer alive (as documented in IAsyncIo).
    dispatch_data_t write_data = dispatch_data_create(
        buffer, size, io_queue_,
        DISPATCH_DATA_DESTRUCTOR_DEFAULT
    );
    if (!write_data) {
        inflight_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

    uint64_t ud = user_data;
    IoCallback cb = std::move(callback);

    off_t file_offset = offset >= 0 ? static_cast<off_t>(offset) : 0;

    dispatch_io_write(
        channel,
        file_offset,
        write_data,
        io_queue_,
        ^(bool done, dispatch_data_t /*data*/, int error) {
            if (done) {
                IoCompletion completion{};
                if (error == 0) {
                    completion.status = IoCompletion::Status::Success;
                    completion.bytes_transferred = size;
                } else if (error == ECANCELED) {
                    completion.status = IoCompletion::Status::Cancelled;
                    completion.error_code = error;
                } else {
                    completion.status = IoCompletion::Status::Error;
                    completion.error_code = error;
                }
                completion.user_data = ud;

                enqueue_completion(completion, cb);
                inflight_.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    );

    dispatch_release(write_data);
    return true;
}

void DispatchIoAsyncIo::enqueue_completion(IoCompletion completion, IoCallback callback) {
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        completions_.push_back(CompletionEntry{completion, std::move(callback)});
    }
    dispatch_semaphore_signal(completion_sem_);
}

uint32_t DispatchIoAsyncIo::drain_completions() {
    std::vector<CompletionEntry> batch;
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        batch.swap(completions_);
    }

    for (auto& entry : batch) {
        if (entry.callback) {
            entry.callback(entry.completion);
        }
    }

    return static_cast<uint32_t>(batch.size());
}

uint32_t DispatchIoAsyncIo::poll() {
    if (!initialized_) return 0;
    return drain_completions();
}

uint32_t DispatchIoAsyncIo::wait(std::chrono::milliseconds timeout) {
    if (!initialized_) return 0;

    // First try a non-blocking drain
    uint32_t completed = drain_completions();
    if (completed > 0) return completed;

    // Block on the semaphore until a completion arrives
    dispatch_time_t deadline;
    if (timeout.count() == 0) {
        deadline = DISPATCH_TIME_FOREVER;
    } else {
        deadline = dispatch_time(DISPATCH_TIME_NOW,
                                 static_cast<int64_t>(timeout.count()) * NSEC_PER_MSEC);
    }

    long result = dispatch_semaphore_wait(completion_sem_, deadline);
    if (result != 0) {
        // Timeout
        return 0;
    }

    // The semaphore was signaled; drain all available completions
    return drain_completions();
}

uint32_t DispatchIoAsyncIo::pending_count() const {
    return inflight_.load(std::memory_order_relaxed);
}

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<IAsyncIo> create_async_io() {
    return std::make_unique<DispatchIoAsyncIo>();
}

} // namespace rex::platform

#endif // __APPLE__
