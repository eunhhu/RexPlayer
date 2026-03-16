#ifdef __linux__

#include "async_io.h"

#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace rex::platform {

// ============================================================================
// io_uring syscall wrappers (avoids liburing dependency)
// ============================================================================

static int io_uring_setup(unsigned entries, struct io_uring_params* p) {
    return static_cast<int>(syscall(__NR_io_uring_setup, entries, p));
}

static int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                          unsigned flags, sigset_t* sig) {
    return static_cast<int>(syscall(__NR_io_uring_enter, fd, to_submit,
                                   min_complete, flags, sig, 0));
}

static int io_uring_register(int fd, unsigned opcode, void* arg,
                             unsigned nr_args) {
    return static_cast<int>(syscall(__NR_io_uring_register, fd, opcode, arg, nr_args));
}

// ============================================================================
// IoUringAsyncIo — io_uring based async I/O for Linux
// ============================================================================

class IoUringAsyncIo : public IAsyncIo {
public:
    IoUringAsyncIo();
    ~IoUringAsyncIo() override;

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
    /// Per-operation context stored until completion
    struct OpContext {
        uint64_t user_data;
        IoCallback callback;
    };

    /// Get the next submission queue entry, or nullptr if full
    struct io_uring_sqe* get_sqe();

    /// Submit all pending SQEs to the kernel
    int submit_pending();

    /// Reap completions from the CQ ring
    uint32_t reap_completions();

    // io_uring file descriptor
    int ring_fd_ = -1;

    // Submission queue (SQ) state
    uint32_t* sq_head_ = nullptr;
    uint32_t* sq_tail_ = nullptr;
    uint32_t* sq_ring_mask_ = nullptr;
    uint32_t* sq_ring_entries_ = nullptr;
    uint32_t* sq_flags_ = nullptr;
    uint32_t* sq_array_ = nullptr;
    struct io_uring_sqe* sqes_ = nullptr;

    // Completion queue (CQ) state
    uint32_t* cq_head_ = nullptr;
    uint32_t* cq_tail_ = nullptr;
    uint32_t* cq_ring_mask_ = nullptr;
    uint32_t* cq_ring_entries_ = nullptr;
    struct io_uring_cqe* cqes_ = nullptr;

    // Memory mappings
    void* sq_ring_ptr_ = nullptr;
    size_t sq_ring_size_ = 0;
    void* cq_ring_ptr_ = nullptr;
    size_t cq_ring_size_ = 0;
    void* sqes_ptr_ = nullptr;
    size_t sqes_size_ = 0;

    // Operation tracking
    std::mutex ops_mutex_;
    uint64_t next_op_id_ = 1;
    std::unordered_map<uint64_t, OpContext> pending_ops_;
    std::atomic<uint32_t> inflight_{0};
    uint32_t sq_pending_ = 0;

    bool initialized_ = false;
};

// ============================================================================
// Implementation
// ============================================================================

IoUringAsyncIo::IoUringAsyncIo() = default;

IoUringAsyncIo::~IoUringAsyncIo() {
    shutdown();
}

bool IoUringAsyncIo::initialize(uint32_t queue_depth) {
    if (initialized_) return false;

    struct io_uring_params params{};
    std::memset(&params, 0, sizeof(params));

    ring_fd_ = io_uring_setup(queue_depth, &params);
    if (ring_fd_ < 0) {
        return false;
    }

    // Map the submission queue ring buffer
    sq_ring_size_ = params.sq_off.array + params.sq_entries * sizeof(uint32_t);
    sq_ring_ptr_ = mmap(nullptr, sq_ring_size_, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, ring_fd_,
                        IORING_OFF_SQ_RING);
    if (sq_ring_ptr_ == MAP_FAILED) {
        close(ring_fd_);
        ring_fd_ = -1;
        return false;
    }

    auto* sq_base = static_cast<uint8_t*>(sq_ring_ptr_);
    sq_head_         = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.head);
    sq_tail_         = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.tail);
    sq_ring_mask_    = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.ring_mask);
    sq_ring_entries_ = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.ring_entries);
    sq_flags_        = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.flags);
    sq_array_        = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.array);

    // Map the SQE array
    sqes_size_ = params.sq_entries * sizeof(struct io_uring_sqe);
    sqes_ptr_ = mmap(nullptr, sqes_size_, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, ring_fd_,
                     IORING_OFF_SQES);
    if (sqes_ptr_ == MAP_FAILED) {
        munmap(sq_ring_ptr_, sq_ring_size_);
        close(ring_fd_);
        ring_fd_ = -1;
        return false;
    }
    sqes_ = static_cast<struct io_uring_sqe*>(sqes_ptr_);

    // Map the completion queue ring buffer
    // If CQ shares the SQ ring mapping (IORING_FEAT_SINGLE_MMAP), reuse it
    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        cq_ring_ptr_ = sq_ring_ptr_;
        cq_ring_size_ = sq_ring_size_;
    } else {
        cq_ring_size_ = params.cq_off.cqes +
                        params.cq_entries * sizeof(struct io_uring_cqe);
        cq_ring_ptr_ = mmap(nullptr, cq_ring_size_, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, ring_fd_,
                            IORING_OFF_CQ_RING);
        if (cq_ring_ptr_ == MAP_FAILED) {
            munmap(sqes_ptr_, sqes_size_);
            munmap(sq_ring_ptr_, sq_ring_size_);
            close(ring_fd_);
            ring_fd_ = -1;
            return false;
        }
    }

    auto* cq_base = static_cast<uint8_t*>(cq_ring_ptr_);
    cq_head_         = reinterpret_cast<uint32_t*>(cq_base + params.cq_off.head);
    cq_tail_         = reinterpret_cast<uint32_t*>(cq_base + params.cq_off.tail);
    cq_ring_mask_    = reinterpret_cast<uint32_t*>(cq_base + params.cq_off.ring_mask);
    cq_ring_entries_ = reinterpret_cast<uint32_t*>(cq_base + params.cq_off.ring_entries);
    cqes_ = reinterpret_cast<struct io_uring_cqe*>(cq_base + params.cq_off.cqes);

    initialized_ = true;
    return true;
}

void IoUringAsyncIo::shutdown() {
    if (!initialized_) return;

    if (sqes_ptr_ && sqes_ptr_ != MAP_FAILED) {
        munmap(sqes_ptr_, sqes_size_);
    }
    if (cq_ring_ptr_ && cq_ring_ptr_ != MAP_FAILED && cq_ring_ptr_ != sq_ring_ptr_) {
        munmap(cq_ring_ptr_, cq_ring_size_);
    }
    if (sq_ring_ptr_ && sq_ring_ptr_ != MAP_FAILED) {
        munmap(sq_ring_ptr_, sq_ring_size_);
    }
    if (ring_fd_ >= 0) {
        close(ring_fd_);
    }

    sq_ring_ptr_ = nullptr;
    cq_ring_ptr_ = nullptr;
    sqes_ptr_ = nullptr;
    ring_fd_ = -1;
    initialized_ = false;
    inflight_.store(0);

    std::lock_guard<std::mutex> lock(ops_mutex_);
    pending_ops_.clear();
}

bool IoUringAsyncIo::register_handle(IoHandle handle) {
    if (!initialized_) return false;
    // io_uring supports registered file descriptors for faster submission.
    // For simplicity, we use direct fd references (no IORING_REGISTER_FILES).
    // The handle is valid as-is.
    (void)handle;
    return true;
}

void IoUringAsyncIo::unregister_handle(IoHandle /*handle*/) {
    // No-op when not using IORING_REGISTER_FILES
}

struct io_uring_sqe* IoUringAsyncIo::get_sqe() {
    uint32_t head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
    uint32_t tail = *sq_tail_;
    uint32_t mask = *sq_ring_mask_;

    if (tail - head >= *sq_ring_entries_) {
        // SQ is full; try submitting pending entries first
        if (submit_pending() < 0) return nullptr;
        head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
        if (tail - head >= *sq_ring_entries_) {
            return nullptr;
        }
    }

    uint32_t index = tail & mask;
    sq_array_[index] = index;
    struct io_uring_sqe* sqe = &sqes_[index];
    std::memset(sqe, 0, sizeof(*sqe));

    *sq_tail_ = tail + 1;
    __atomic_store_n(sq_tail_, tail + 1, __ATOMIC_RELEASE);
    ++sq_pending_;

    return sqe;
}

int IoUringAsyncIo::submit_pending() {
    if (sq_pending_ == 0) return 0;
    int ret = io_uring_enter(ring_fd_, sq_pending_, 0, 0, nullptr);
    if (ret >= 0) {
        sq_pending_ = 0;
    }
    return ret;
}

bool IoUringAsyncIo::submit_read(IoHandle handle, void* buffer, size_t size,
                                  int64_t offset, uint64_t user_data,
                                  IoCallback callback) {
    if (!initialized_) return false;

    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) return false;

    uint64_t op_id;
    {
        std::lock_guard<std::mutex> lock(ops_mutex_);
        op_id = next_op_id_++;
        pending_ops_[op_id] = OpContext{user_data, std::move(callback)};
    }

    sqe->opcode = IORING_OP_READ;
    sqe->fd = handle;
    sqe->addr = reinterpret_cast<uint64_t>(buffer);
    sqe->len = static_cast<uint32_t>(size);
    sqe->off = static_cast<uint64_t>(offset >= 0 ? offset : static_cast<uint64_t>(-1));
    sqe->user_data = op_id;

    inflight_.fetch_add(1, std::memory_order_relaxed);

    // Submit immediately
    if (submit_pending() < 0) {
        std::lock_guard<std::mutex> lock(ops_mutex_);
        pending_ops_.erase(op_id);
        inflight_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

    return true;
}

bool IoUringAsyncIo::submit_write(IoHandle handle, const void* buffer, size_t size,
                                   int64_t offset, uint64_t user_data,
                                   IoCallback callback) {
    if (!initialized_) return false;

    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) return false;

    uint64_t op_id;
    {
        std::lock_guard<std::mutex> lock(ops_mutex_);
        op_id = next_op_id_++;
        pending_ops_[op_id] = OpContext{user_data, std::move(callback)};
    }

    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = handle;
    sqe->addr = reinterpret_cast<uint64_t>(buffer);
    sqe->len = static_cast<uint32_t>(size);
    sqe->off = static_cast<uint64_t>(offset >= 0 ? offset : static_cast<uint64_t>(-1));
    sqe->user_data = op_id;

    inflight_.fetch_add(1, std::memory_order_relaxed);

    if (submit_pending() < 0) {
        std::lock_guard<std::mutex> lock(ops_mutex_);
        pending_ops_.erase(op_id);
        inflight_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

    return true;
}

uint32_t IoUringAsyncIo::reap_completions() {
    uint32_t completed = 0;

    uint32_t head = __atomic_load_n(cq_head_, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
    uint32_t mask = *cq_ring_mask_;

    while (head != tail) {
        struct io_uring_cqe* cqe = &cqes_[head & mask];

        uint64_t op_id = cqe->user_data;
        int32_t res = cqe->res;

        OpContext ctx;
        {
            std::lock_guard<std::mutex> lock(ops_mutex_);
            auto it = pending_ops_.find(op_id);
            if (it != pending_ops_.end()) {
                ctx = std::move(it->second);
                pending_ops_.erase(it);
            }
        }

        if (ctx.callback) {
            IoCompletion completion{};
            if (res >= 0) {
                completion.status = IoCompletion::Status::Success;
                completion.bytes_transferred = static_cast<size_t>(res);
            } else if (res == -ECANCELED) {
                completion.status = IoCompletion::Status::Cancelled;
                completion.error_code = -res;
            } else {
                completion.status = IoCompletion::Status::Error;
                completion.error_code = -res;
            }
            completion.user_data = ctx.user_data;
            ctx.callback(completion);
        }

        inflight_.fetch_sub(1, std::memory_order_relaxed);
        ++head;
        ++completed;
    }

    __atomic_store_n(cq_head_, head, __ATOMIC_RELEASE);
    return completed;
}

uint32_t IoUringAsyncIo::poll() {
    if (!initialized_) return 0;
    return reap_completions();
}

uint32_t IoUringAsyncIo::wait(std::chrono::milliseconds timeout) {
    if (!initialized_) return 0;

    // First try a non-blocking reap
    uint32_t completed = reap_completions();
    if (completed > 0) return completed;

    // Block until at least one completion
    unsigned flags = IORING_ENTER_GETEVENTS;

    // io_uring_enter with min_complete=1 blocks until at least one CQE is available.
    // For timeout support, we'd use IORING_OP_TIMEOUT linked SQE, but for
    // simplicity we use the basic blocking mode.
    // A timeout of 0 means wait indefinitely.
    (void)timeout; // TODO: use __kernel_timespec with IORING_ENTER_EXT_ARG for timeout

    int ret = io_uring_enter(ring_fd_, 0, 1, flags, nullptr);
    if (ret < 0) return 0;

    return reap_completions();
}

uint32_t IoUringAsyncIo::pending_count() const {
    return inflight_.load(std::memory_order_relaxed);
}

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<IAsyncIo> create_async_io() {
    return std::make_unique<IoUringAsyncIo>();
}

} // namespace rex::platform

#endif // __linux__
