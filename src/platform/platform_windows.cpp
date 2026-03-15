#ifdef _WIN32

#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <processthreadsapi.h>

namespace rex::platform {

void* alloc_aligned(size_t size, size_t alignment) {
    return _aligned_malloc(size, alignment);
}

void free_aligned(void* ptr, size_t /*size*/) {
    _aligned_free(ptr);
}

uint32_t cpu_count() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<uint32_t>(si.dwNumberOfProcessors);
}

bool pin_thread_to_core(uint32_t core_id) {
    DWORD_PTR mask = 1ULL << core_id;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
}

size_t page_size() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<size_t>(si.dwPageSize);
}

uint64_t timestamp_ns() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart * 1000000000ULL / freq.QuadPart);
}

void set_thread_name(const std::string& name) {
    // Windows 10 1607+ SetThreadDescription
    int size = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    if (size > 0) {
        std::wstring wname(size, 0);
        MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname.data(), size);
        SetThreadDescription(GetCurrentThread(), wname.c_str());
    }
}

} // namespace rex::platform

#endif // _WIN32
