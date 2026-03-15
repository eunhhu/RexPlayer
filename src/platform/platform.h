#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace rex::platform {

/// Allocate aligned memory (page-aligned for VM use)
void* alloc_aligned(size_t size, size_t alignment = 4096);

/// Free aligned memory
void free_aligned(void* ptr, size_t size);

/// Get the number of available CPU cores
uint32_t cpu_count();

/// Pin the current thread to a specific CPU core
bool pin_thread_to_core(uint32_t core_id);

/// Get the system page size
size_t page_size();

/// High-resolution timestamp in nanoseconds
uint64_t timestamp_ns();

/// Set a thread name (for debugging)
void set_thread_name(const std::string& name);

} // namespace rex::platform
