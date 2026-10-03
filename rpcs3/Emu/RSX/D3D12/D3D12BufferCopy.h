#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <immintrin.h>

// Upload buffers are normally aligned; retain a safe path for arbitrary output
// and copy all tail bytes. Publish non-temporal stores before returning.
inline void streamToBuffer(void* dst, const void* src, size_t sizeInBytes)
{
    if (!sizeInBytes) return;
    if (reinterpret_cast<uintptr_t>(dst) & 15)
    {
        std::memcpy(dst, src, sizeInBytes);
        return;
    }
    const size_t streamed = sizeInBytes & ~size_t{15};
    for (size_t offset = 0; offset < streamed; offset += 16)
        _mm_stream_si128(reinterpret_cast<__m128i*>(static_cast<char*>(dst) + offset),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<const char*>(src) + offset)));
    if (streamed != sizeInBytes)
        std::memcpy(static_cast<char*>(dst) + streamed, static_cast<const char*>(src) + streamed, sizeInBytes - streamed);
    if (streamed) _mm_sfence();
}

inline void streamBuffer(void* dst, const void* src, size_t sizeInBytes)
{
    // The legacy unsigned size-64 loop underflowed for small transfers.
    streamToBuffer(dst, src, sizeInBytes);
}
