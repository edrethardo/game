#pragma once

#include "core/types.h"

namespace AllocationTracker {
    void init();
    void shutdown();
    void resetFrameCount();

    u32 getFrameAllocCount();
    u64 getTotalAllocCount();
    u64 getTotalBytesAllocated();
}
