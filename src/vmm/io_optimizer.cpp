#include "io_optimizer.h"
#include <algorithm>

namespace rex::vmm {

IoOptimizer::IoOptimizer() = default;

void IoOptimizer::set_batch_config(const BatchConfig& config) {
    std::lock_guard lock(mutex_);
    batch_config_ = config;
}

void IoOptimizer::record_read(uint64_t bytes, double latency_us) {
    read_ops_.fetch_add(1, std::memory_order_relaxed);
    bytes_read_.fetch_add(bytes, std::memory_order_relaxed);

    std::lock_guard lock(mutex_);
    uint64_t ops = read_ops_.load(std::memory_order_relaxed);
    // Exponential moving average (α = 0.1)
    total_read_latency_ = total_read_latency_ * 0.9 + latency_us * 0.1;
    (void)ops;
}

void IoOptimizer::record_write(uint64_t bytes, double latency_us) {
    write_ops_.fetch_add(1, std::memory_order_relaxed);
    bytes_written_.fetch_add(bytes, std::memory_order_relaxed);

    std::lock_guard lock(mutex_);
    total_write_latency_ = total_write_latency_ * 0.9 + latency_us * 0.1;
}

void IoOptimizer::record_notification_suppressed() {
    notifications_suppressed_.fetch_add(1, std::memory_order_relaxed);
}

void IoOptimizer::record_batch(uint32_t batch_size) {
    batched_ops_.fetch_add(batch_size, std::memory_order_relaxed);
}

IoStats IoOptimizer::get_stats() const {
    std::lock_guard lock(mutex_);
    IoStats stats{};
    stats.read_ops = read_ops_.load(std::memory_order_relaxed);
    stats.write_ops = write_ops_.load(std::memory_order_relaxed);
    stats.bytes_read = bytes_read_.load(std::memory_order_relaxed);
    stats.bytes_written = bytes_written_.load(std::memory_order_relaxed);
    stats.avg_read_latency_us = total_read_latency_;
    stats.avg_write_latency_us = total_write_latency_;
    stats.notifications_suppressed = notifications_suppressed_.load(std::memory_order_relaxed);
    stats.batched_ops = batched_ops_.load(std::memory_order_relaxed);
    return stats;
}

void IoOptimizer::reset_stats() {
    std::lock_guard lock(mutex_);
    read_ops_.store(0, std::memory_order_relaxed);
    write_ops_.store(0, std::memory_order_relaxed);
    bytes_read_.store(0, std::memory_order_relaxed);
    bytes_written_.store(0, std::memory_order_relaxed);
    notifications_suppressed_.store(0, std::memory_order_relaxed);
    batched_ops_.store(0, std::memory_order_relaxed);
    total_read_latency_ = 0.0;
    total_write_latency_ = 0.0;
}

bool IoOptimizer::should_notify(uint32_t pending_ops) const {
    if (!batch_config_.event_idx) return true;
    // Notify when batch is full or when enough ops have accumulated
    return pending_ops >= batch_config_.max_batch_size;
}

uint32_t IoOptimizer::recommended_batch_size() const {
    // Adaptive batch sizing based on current I/O rate
    uint64_t total_ops = read_ops_.load(std::memory_order_relaxed)
                       + write_ops_.load(std::memory_order_relaxed);

    if (total_ops < 1000) return 8;    // Low I/O: small batches for latency
    if (total_ops < 10000) return 32;  // Medium: balance
    return batch_config_.max_batch_size; // High: max throughput
}

} // namespace rex::vmm
