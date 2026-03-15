#ifdef __APPLE__

#include "platform.h"

#include <cstdlib>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/sysctl.h>
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
    int count = 0;
    size_t len = sizeof(count);
    sysctlbyname("hw.ncpu", &count, &len, nullptr, 0);
    return count > 0 ? static_cast<uint32_t>(count) : 1;
}

bool pin_thread_to_core(uint32_t /*core_id*/) {
    // macOS doesn't support thread affinity pinning directly
    // thread_policy_set with THREAD_AFFINITY_POLICY is a hint only
    return false;
}

size_t page_size() {
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
}

uint64_t timestamp_ns() {
    static mach_timebase_info_data_t timebase = {};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    uint64_t ticks = mach_absolute_time();
    return ticks * timebase.numer / timebase.denom;
}

void set_thread_name(const std::string& name) {
    pthread_setname_np(name.substr(0, 63).c_str());
}

} // namespace rex::platform

#endif // __APPLE__
