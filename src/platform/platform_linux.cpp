#ifdef __linux__

#include "platform.h"

#include <cstdlib>
#include <ctime>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

namespace rex::platform {

void* alloc_aligned(size_t size, size_t alignment) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
}

void free_aligned(void* ptr, size_t /*size*/) {
    free(ptr);
}

uint32_t cpu_count() {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? static_cast<uint32_t>(count) : 1;
}

bool pin_thread_to_core(uint32_t core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
}

size_t page_size() {
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
}

uint64_t timestamp_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

void set_thread_name(const std::string& name) {
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

} // namespace rex::platform

#endif // __linux__
