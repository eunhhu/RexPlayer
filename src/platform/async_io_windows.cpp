#ifdef _WIN32

#include "async_io.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rex::platform {

// ============================================================================
// IocpAsyncIo — I/O Completion Ports based async I/O for Windows
// ============================================================================

class IocpAsyncIo : public IAsyncIo {
public:
    IocpAsyncIo();
    ~IocpAsyncIo() override;

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
    /// Extended OVERLAPPED structure carrying per-operation context
    struct OverlappedEx {
        OVERLAPPED overlapped;        // Must be first member
        uint64_t user_data;
        IoCallback callback;
        bool is_write;

        OverlappedEx() {
            std::memset(&overlapped, 0, sizeof(overlapped));
            user_data = 0;
            is_write = false;
        }
    };

    /// Dequeue and process completions from IOCP
    /// @param timeout_ms  INFINITE for blocking, 0 for non-blocking
    /// @param max_events  Maximum events to process in one call
    /// @return Number of completions processed
    uint32_t process_completions(DWORD timeout_ms, uint32_t max_events);

    HANDLE iocp_ = INVALID_HANDLE_VALUE;
    std::atomic<uint32_t> inflight_{0};
    uint32_t queue_depth_ = 256;
    bool initialized_ = false;

    // Track allocated OverlappedEx structs for cleanup on shutdown
    std::mutex overlapped_mutex_;
    std::vector<OverlappedEx*> active_overlapped_;
};

// ============================================================================
// Implementation
// ============================================================================

IocpAsyncIo::IocpAsyncIo() = default;

IocpAsyncIo::~IocpAsyncIo() {
    shutdown();
}

bool IocpAsyncIo::initialize(uint32_t queue_depth) {
    if (initialized_) return false;

    queue_depth_ = queue_depth;

    // Create the I/O Completion Port
    // NumberOfConcurrentThreads = 0 means one thread per CPU core
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (iocp_ == nullptr || iocp_ == INVALID_HANDLE_VALUE) {
        iocp_ = INVALID_HANDLE_VALUE;
        return false;
    }

    initialized_ = true;
    return true;
}

void IocpAsyncIo::shutdown() {
    if (!initialized_) return;

    // Cancel all pending I/O — completions will arrive with ERROR_OPERATION_ABORTED
    // Process remaining completions with a short timeout
    process_completions(100, queue_depth_);

    // Free any remaining OverlappedEx structs
    {
        std::lock_guard<std::mutex> lock(overlapped_mutex_);
        for (auto* ovl : active_overlapped_) {
            delete ovl;
        }
        active_overlapped_.clear();
    }

    if (iocp_ != INVALID_HANDLE_VALUE) {
        CloseHandle(iocp_);
        iocp_ = INVALID_HANDLE_VALUE;
    }

    inflight_.store(0);
    initialized_ = false;
}

bool IocpAsyncIo::register_handle(IoHandle handle) {
    if (!initialized_) return false;

    // Associate the file HANDLE with our IOCP
    // The completion key is the HANDLE itself (cast to ULONG_PTR)
    HANDLE result = CreateIoCompletionPort(
        handle,
        iocp_,
        reinterpret_cast<ULONG_PTR>(handle),
        0
    );

    return result != nullptr;
}

void IocpAsyncIo::unregister_handle(IoHandle handle) {
    // Windows does not provide a way to disassociate a handle from an IOCP.
    // Cancel any pending I/O on this handle instead.
    if (initialized_) {
        CancelIo(handle);
    }
}

bool IocpAsyncIo::submit_read(IoHandle handle, void* buffer, size_t size,
                                int64_t offset, uint64_t user_data,
                                IoCallback callback) {
    if (!initialized_) return false;

    auto* ovl = new OverlappedEx();
    ovl->user_data = user_data;
    ovl->callback = std::move(callback);
    ovl->is_write = false;

    // Set file offset in the OVERLAPPED structure
    if (offset >= 0) {
        ovl->overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
        ovl->overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
    }
    // If offset == -1, leave Offset/OffsetHigh as 0 (read from current position)

    {
        std::lock_guard<std::mutex> lock(overlapped_mutex_);
        active_overlapped_.push_back(ovl);
    }

    inflight_.fetch_add(1, std::memory_order_relaxed);

    DWORD bytes_read = 0;
    BOOL result = ReadFile(
        handle,
        buffer,
        static_cast<DWORD>(size),
        &bytes_read,
        &ovl->overlapped
    );

    if (!result) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            // Immediate failure — not queued
            inflight_.fetch_sub(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(overlapped_mutex_);
                active_overlapped_.erase(
                    std::remove(active_overlapped_.begin(), active_overlapped_.end(), ovl),
                    active_overlapped_.end()
                );
            }

            // Invoke callback with error
            if (ovl->callback) {
                IoCompletion completion{};
                completion.status = IoCompletion::Status::Error;
                completion.error_code = static_cast<int32_t>(err);
                completion.user_data = user_data;
                ovl->callback(completion);
            }

            delete ovl;
            return false;
        }
        // ERROR_IO_PENDING — operation is in progress, completion will arrive on IOCP
    }
    // If result == TRUE, the operation completed synchronously but
    // the completion packet is still posted to the IOCP (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
    // is not set), so we handle it the same way.

    return true;
}

bool IocpAsyncIo::submit_write(IoHandle handle, const void* buffer, size_t size,
                                 int64_t offset, uint64_t user_data,
                                 IoCallback callback) {
    if (!initialized_) return false;

    auto* ovl = new OverlappedEx();
    ovl->user_data = user_data;
    ovl->callback = std::move(callback);
    ovl->is_write = true;

    if (offset >= 0) {
        ovl->overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
        ovl->overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
    }

    {
        std::lock_guard<std::mutex> lock(overlapped_mutex_);
        active_overlapped_.push_back(ovl);
    }

    inflight_.fetch_add(1, std::memory_order_relaxed);

    DWORD bytes_written = 0;
    BOOL result = WriteFile(
        handle,
        buffer,
        static_cast<DWORD>(size),
        &bytes_written,
        &ovl->overlapped
    );

    if (!result) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            inflight_.fetch_sub(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(overlapped_mutex_);
                active_overlapped_.erase(
                    std::remove(active_overlapped_.begin(), active_overlapped_.end(), ovl),
                    active_overlapped_.end()
                );
            }

            if (ovl->callback) {
                IoCompletion completion{};
                completion.status = IoCompletion::Status::Error;
                completion.error_code = static_cast<int32_t>(err);
                completion.user_data = user_data;
                ovl->callback(completion);
            }

            delete ovl;
            return false;
        }
    }

    return true;
}

uint32_t IocpAsyncIo::process_completions(DWORD timeout_ms, uint32_t max_events) {
    uint32_t completed = 0;

    // Use GetQueuedCompletionStatusEx for batch dequeue when available
    std::vector<OVERLAPPED_ENTRY> entries(max_events);
    ULONG num_entries = 0;

    BOOL result = GetQueuedCompletionStatusEx(
        iocp_,
        entries.data(),
        static_cast<ULONG>(entries.size()),
        &num_entries,
        timeout_ms,
        FALSE  // not alertable
    );

    if (!result) {
        // Timeout or error
        return 0;
    }

    for (ULONG i = 0; i < num_entries; ++i) {
        auto* ovl = reinterpret_cast<OverlappedEx*>(entries[i].lpOverlapped);
        if (!ovl) continue;

        DWORD bytes = entries[i].dwNumberOfBytesTransferred;
        DWORD err = 0;

        // Check if the operation succeeded
        // For failed operations, we need to retrieve the error via GetOverlappedResult
        DWORD transferred = 0;
        BOOL op_result = GetOverlappedResult(
            reinterpret_cast<HANDLE>(entries[i].lpCompletionKey),
            &ovl->overlapped,
            &transferred,
            FALSE
        );

        if (!op_result) {
            err = GetLastError();
        } else {
            bytes = transferred;
        }

        if (ovl->callback) {
            IoCompletion completion{};
            if (op_result) {
                completion.status = IoCompletion::Status::Success;
                completion.bytes_transferred = static_cast<size_t>(bytes);
            } else if (err == ERROR_OPERATION_ABORTED) {
                completion.status = IoCompletion::Status::Cancelled;
                completion.error_code = static_cast<int32_t>(err);
            } else {
                completion.status = IoCompletion::Status::Error;
                completion.error_code = static_cast<int32_t>(err);
            }
            completion.user_data = ovl->user_data;
            ovl->callback(completion);
        }

        inflight_.fetch_sub(1, std::memory_order_relaxed);

        // Remove from tracking and delete
        {
            std::lock_guard<std::mutex> lock(overlapped_mutex_);
            active_overlapped_.erase(
                std::remove(active_overlapped_.begin(), active_overlapped_.end(), ovl),
                active_overlapped_.end()
            );
        }
        delete ovl;

        ++completed;
    }

    return completed;
}

uint32_t IocpAsyncIo::poll() {
    if (!initialized_) return 0;
    return process_completions(0, queue_depth_);
}

uint32_t IocpAsyncIo::wait(std::chrono::milliseconds timeout) {
    if (!initialized_) return 0;

    DWORD timeout_ms = (timeout.count() == 0) ? INFINITE : static_cast<DWORD>(timeout.count());
    return process_completions(timeout_ms, queue_depth_);
}

uint32_t IocpAsyncIo::pending_count() const {
    return inflight_.load(std::memory_order_relaxed);
}

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<IAsyncIo> create_async_io() {
    return std::make_unique<IocpAsyncIo>();
}

} // namespace rex::platform

#endif // _WIN32
