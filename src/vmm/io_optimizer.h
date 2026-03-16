#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rex::vmm {

/// I/O performance statistics
struct IoStats {
    /// Total read operations
    uint64_t read_ops = 0;
    /// Total write operations
    uint64_t write_ops = 0;
    /// Total bytes read
    uint64_t bytes_read = 0;
    /// Total bytes written
    uint64_t bytes_written = 0;
    /// Average read latency (microseconds)
    double avg_read_latency_us = 0.0;
    /// Average write latency (microseconds)
    double avg_write_latency_us = 0.0;
    /// Virtqueue notifications suppressed (VIRTIO_F_EVENT_IDX)
    uint64_t notifications_suppressed = 0;
    /// Batched operations count
    uint64_t batched_ops = 0;
};

/// Virtqueue batch processing configuration
struct BatchConfig {
    /// Maximum number of descriptors to process in one batch
    uint32_t max_batch_size = 64;
    /// Maximum time to wait for batch accumulation (microseconds)
    uint32_t batch_timeout_us = 100;
    /// Enable VIRTIO_F_EVENT_IDX interrupt suppression
    bool event_idx = true;
};

/// I/O optimization manager
///
/// Provides batched virtqueue processing, interrupt coalescing,
/// and I/O statistics tracking for performance monitoring.
class IoOptimizer {
public:
    IoOptimizer();

    /// Configure batch processing
    void set_batch_config(const BatchConfig& config);
    const BatchConfig& batch_config() const { return batch_config_; }

    /// Record a read operation
    void record_read(uint64_t bytes, double latency_us);

    /// Record a write operation
    void record_write(uint64_t bytes, double latency_us);

    /// Record a suppressed notification
    void record_notification_suppressed();

    /// Record a batched operation
    void record_batch(uint32_t batch_size);

    /// Get current I/O statistics
    IoStats get_stats() const;

    /// Reset all statistics
    void reset_stats();

    /// Check if an interrupt should be sent based on coalescing
    /// Returns true if enough operations have accumulated
    bool should_notify(uint32_t pending_ops) const;

    /// Get the recommended batch size based on current load
    uint32_t recommended_batch_size() const;

private:
    BatchConfig batch_config_;
    mutable std::mutex mutex_;

    std::atomic<uint64_t> read_ops_{0};
    std::atomic<uint64_t> write_ops_{0};
    std::atomic<uint64_t> bytes_read_{0};
    std::atomic<uint64_t> bytes_written_{0};
    std::atomic<uint64_t> notifications_suppressed_{0};
    std::atomic<uint64_t> batched_ops_{0};

    // Latency tracking (rolling averages)
    double total_read_latency_ = 0.0;
    double total_write_latency_ = 0.0;
};

} // namespace rex::vmm
