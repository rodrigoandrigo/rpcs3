#pragma once
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

// Storage remains owned until XAudio2's OnBufferEnd notification. Allocation
// and resizing happen only after the previous source voice is destroyed.
class XAudio2BufferPool
{
public:
    // Keep one processing quantum ahead, using only real producer samples.
    static std::size_t request_bytes(std::size_t needed, std::size_t capacity,
        std::size_t frame_bytes, std::size_t rate) noexcept
    {
        if (!frame_bytes) return 0;
        const auto headroom = (rate / 100) * frame_bytes;
        const auto request = needed >= capacity ? capacity : needed + std::min(headroom, capacity - needed);
        return request - request % frame_bytes;
    }
    struct slot
    {
        std::vector<std::uint8_t> data;
        std::atomic<bool> in_use{false};
    };

    void initialize(std::size_t bytes)
    {
        for (auto& buffer : m_buffers) {
            buffer.data.resize(bytes);
            buffer.in_use.store(false, std::memory_order_relaxed);
        }
    }

    slot* acquire() noexcept
    {
        for (auto& buffer : m_buffers) {
            bool free = false;
            if (buffer.in_use.compare_exchange_strong(free, true, std::memory_order_acquire))
                return &buffer;
        }
        return nullptr;
    }

    static void release(void* context) noexcept
    {
        if (context) static_cast<slot*>(context)->in_use.store(false, std::memory_order_release);
    }

private:
    std::array<slot, 3> m_buffers;
};
