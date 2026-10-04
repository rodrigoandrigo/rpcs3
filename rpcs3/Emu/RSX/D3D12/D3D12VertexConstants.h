#pragma once
#include <cstdint>
#include <span>

namespace d3d12
{
// Empty means the full 468-register bank. Indexed shaders retain RSX indices;
// non-indexed shaders use the sorted compact table emitted by the decompiler.
inline std::span<const std::uint16_t> vertex_constant_upload_ids(
    bool indexed, std::span<const std::uint16_t> ids) noexcept
{
    return indexed ? std::span<const std::uint16_t>{} : ids;
}
}
