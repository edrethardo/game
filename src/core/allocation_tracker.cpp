#include "core/allocation_tracker.h"

#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <new>

#ifdef ENGINE_DEBUG

static std::atomic<u32> s_frameAllocCount{0};
static std::atomic<u64> s_totalAllocCount{0};
static std::atomic<u64> s_liveBytes{0};
static bool s_active = false;

void AllocationTracker::init() {
    s_frameAllocCount = 0;
    s_totalAllocCount = 0;
    s_liveBytes = 0;
    s_active = true;
}

void AllocationTracker::shutdown() {
    if (s_liveBytes > 0) {
        fprintf(stderr, "[WARN] AllocationTracker: %llu bytes still live at shutdown\n",
                (unsigned long long)s_liveBytes.load());
    }
    s_active = false;
}

void AllocationTracker::resetFrameCount() {
    s_frameAllocCount = 0;
}

u32 AllocationTracker::getFrameAllocCount() {
    return s_frameAllocCount.load();
}

u64 AllocationTracker::getTotalAllocCount() {
    return s_totalAllocCount.load();
}

u64 AllocationTracker::getTotalBytesAllocated() {
    return s_liveBytes.load();
}

// Override global new/delete to track allocations
void* operator new(std::size_t size) {
    if (s_active) {
        s_frameAllocCount++;
        s_totalAllocCount++;
        s_liveBytes += size;
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void* operator new[](std::size_t size) {
    if (s_active) {
        s_frameAllocCount++;
        s_totalAllocCount++;
        s_liveBytes += size;
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void operator delete(void* ptr) noexcept {
    // Note: we can't track exact size freed without extra bookkeeping
    // The byte count will drift, but frame alloc count is accurate
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t size) noexcept {
    if (s_active && size > 0) {
        s_liveBytes -= size;
    }
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t size) noexcept {
    if (s_active && size > 0) {
        s_liveBytes -= size;
    }
    std::free(ptr);
}

#else

// Release builds — no-op
void AllocationTracker::init() {}
void AllocationTracker::shutdown() {}
void AllocationTracker::resetFrameCount() {}
u32 AllocationTracker::getFrameAllocCount() { return 0; }
u64 AllocationTracker::getTotalAllocCount() { return 0; }
u64 AllocationTracker::getTotalBytesAllocated() { return 0; }

#endif
