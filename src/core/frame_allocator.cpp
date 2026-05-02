#include "core/frame_allocator.h"
#include "core/log.h"
#include "core/assert.h"
#include <cstdlib>

void FrameAllocator::init(usize sizeBytes) {
    ENGINE_ASSERT(sizeBytes > 0, "FrameAllocator size must be > 0");
    m_buffer = static_cast<u8*>(std::malloc(sizeBytes));
    ENGINE_ASSERT(m_buffer != nullptr, "FrameAllocator malloc failed");
    m_capacity = sizeBytes;
    m_offset = 0;
    LOG_INFO("FrameAllocator initialized: %zu bytes", sizeBytes);
}

void FrameAllocator::shutdown() {
    if (m_buffer) {
        std::free(m_buffer);
        m_buffer = nullptr;
    }
    m_capacity = 0;
    m_offset = 0;
}

void FrameAllocator::reset() {
    m_offset = 0;
}

void* FrameAllocator::alloc(usize bytes, usize alignment) {
    // Align offset
    usize aligned = (m_offset + alignment - 1) & ~(alignment - 1);

    if (aligned + bytes > m_capacity) {
        LOG_ERROR("FrameAllocator exhausted: requested %zu bytes, %zu/%zu used",
                  bytes, m_offset, m_capacity);
        return nullptr;
    }

    void* ptr = m_buffer + aligned;
    m_offset = aligned + bytes;
    return ptr;
}
