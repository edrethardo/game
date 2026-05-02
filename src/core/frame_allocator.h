#pragma once

#include "core/types.h"

class FrameAllocator {
public:
    void init(usize sizeBytes);
    void shutdown();
    void reset();

    void* alloc(usize bytes, usize alignment = 16);

    usize getUsed() const { return m_offset; }
    usize getCapacity() const { return m_capacity; }

private:
    u8* m_buffer = nullptr;
    usize m_capacity = 0;
    usize m_offset = 0;
};
