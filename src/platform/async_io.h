#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <chrono>

namespace rex::platform {

/// Result of a completed I/O operation
struct IoCompletion {
    enum class Status : uint8_t {
        Success,
        Error,
        Cancelled,
        Timeout,
    };

    Status status = Status::Error;
    int32_t error_code = 0;      // Platform-specific error code (errno, GetLastError, etc.)
    size_t bytes_transferred = 0;
    uint64_t user_data = 0;      // Opaque value passed by the caller at submit time
};

/// Callback invoked when an async I/O operation completes
using IoCallback = std::function<void(const IoCompletion&)>;

/// File descriptor / handle abstraction
#ifdef _WIN32
using IoHandle = void*;  // HANDLE
#else
using IoHandle = int;     // fd
#endif

/// Interface for platform-specific asynchronous I/O
///
/// Implementations:
///   - Linux: io_uring (kernel 5.1+)
///   - macOS: Grand Central Dispatch (dispatch_io)
///   - Windows: I/O Completion Ports (IOCP)
class IAsyncIo {
public:
    virtual ~IAsyncIo() = default;

    /// Initialize the async I/O subsystem
    /// @param queue_depth  Maximum number of in-flight I/O operations
    /// @return true on success
    virtual bool initialize(uint32_t queue_depth = 256) = 0;

    /// Shut down and release all resources
    virtual void shutdown() = 0;

    /// Associate a file descriptor/handle with this I/O engine.
    /// Must be called before submitting operations on this handle.
    /// @param handle   File descriptor (Linux/macOS) or HANDLE (Windows)
    /// @return true on success
    virtual bool register_handle(IoHandle handle) = 0;

    /// Remove a previously registered handle
    virtual void unregister_handle(IoHandle handle) = 0;

    /// Submit an asynchronous read operation
    /// @param handle   File descriptor/handle to read from
    /// @param buffer   Destination buffer (must remain valid until completion)
    /// @param size     Number of bytes to read
    /// @param offset   File offset (-1 for current position / sequential)
    /// @param user_data Opaque value returned in IoCompletion
    /// @param callback Optional callback invoked on completion
    /// @return true if the operation was successfully submitted
    virtual bool submit_read(
        IoHandle handle,
        void* buffer,
        size_t size,
        int64_t offset,
        uint64_t user_data = 0,
        IoCallback callback = nullptr
    ) = 0;

    /// Submit an asynchronous write operation
    /// @param handle   File descriptor/handle to write to
    /// @param buffer   Source buffer (must remain valid until completion)
    /// @param size     Number of bytes to write
    /// @param offset   File offset (-1 for current position / sequential)
    /// @param user_data Opaque value returned in IoCompletion
    /// @param callback Optional callback invoked on completion
    /// @return true if the operation was successfully submitted
    virtual bool submit_write(
        IoHandle handle,
        const void* buffer,
        size_t size,
        int64_t offset,
        uint64_t user_data = 0,
        IoCallback callback = nullptr
    ) = 0;

    /// Poll for completed I/O operations (non-blocking).
    /// Invokes callbacks for any completed operations.
    /// @return Number of completions processed
    virtual uint32_t poll() = 0;

    /// Wait for at least one I/O completion, up to the given timeout.
    /// Invokes callbacks for completed operations.
    /// @param timeout  Maximum time to wait (0 = indefinite)
    /// @return Number of completions processed, or 0 on timeout
    virtual uint32_t wait(std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) = 0;

    /// Get the number of currently in-flight (submitted but not completed) operations
    virtual uint32_t pending_count() const = 0;
};

/// Factory: create the platform-appropriate async I/O engine
std::unique_ptr<IAsyncIo> create_async_io();

} // namespace rex::platform
