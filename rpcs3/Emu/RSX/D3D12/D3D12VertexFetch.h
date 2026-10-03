#pragma once

#include <ostream>

namespace d3d12
{
    // Decode the current draw_command_processor's 64-bit attribute descriptor.
    // Raw bytes retain PS3 byte order; conversion is performed in the shader.
    inline void insert_vertex_fetch(std::ostream& output)
    {
        output << R"(
ByteAddressBuffer vertex_stream : register(t0);
uint vertex_byte(uint offset)
{
    return (vertex_stream.Load(offset & ~3u) >> ((offset & 3u) * 8u)) & 255u;
}
uint vertex_element(uint offset, uint size, bool swap)
{
    uint first = vertex_byte(offset);
    if (size == 1u) return first;
    uint second = vertex_byte(offset + 1u);
    if (size == 2u) return swap ? (first << 8u) | second : first | (second << 8u);
    uint third = vertex_byte(offset + 2u);
    uint fourth = vertex_byte(offset + 3u);
    return swap ? (first << 24u) | (second << 16u) | (third << 8u) | fourth
        : first | (second << 8u) | (third << 16u) | (fourth << 24u);
}
float4 read_location(uint location, uint vertex_id)
{
    uint4 block = input_attributes_blob[location >> 1u];
    uint2 attribute = (location & 1u) != 0u ? block.zw : block.xy;
    uint stride = attribute.x & 255u;
    uint frequency = (attribute.x >> 8u) & 65535u;
    uint type = (attribute.x >> 24u) & 7u;
    uint size = (attribute.x >> 27u) & 7u;
    if (size == 0u || type == 0u) return float4(0., 0., 0., 1.);
    uint index = frequency == 0u ? 0u : ((attribute.y & 0x80000000u) != 0u
        ? (vertex_id + vertex_index_offset) % frequency
        : (vertex_id + vertex_index_offset - vertex_base_index) / frequency);
    uint offset = (attribute.y & 0x1fffffffu) + index * stride;
    bool swap = (attribute.y & 0x20000000u) != 0u;
    uint element_sizes[8] = {0u, 2u, 4u, 2u, 1u, 2u, 4u, 1u};
    float scales[8] = {1., 32767.5, 1., 1., 255., 1., 32767., 1.};
    uint element_size = element_sizes[type];
    uint4 bits = uint4(vertex_element(offset, element_size, swap),
        size > 1u ? vertex_element(offset + element_size, element_size, swap) : 0u,
        size > 2u ? vertex_element(offset + element_size * 2u, element_size, swap) : 0u,
        size > 3u ? vertex_element(offset + element_size * 3u, element_size, swap) : 0u);
    float4 result;
    if (type == 1u || type == 5u)
    {
        result = float4(int4(bits << 16u) >> 16);
        if (type == 1u) result += 0.5;
    }
    else if (type == 2u) result = asfloat(bits);
    else if (type == 3u) result = float4(f16tof32(bits.x), f16tof32(bits.y), f16tof32(bits.z), f16tof32(bits.w));
    else if (element_size == 1u) result = float4(bits);
    else
    {
        uint packed = bits.x;
        int3 unpacked = int3((packed & 2047u) << 5u, ((packed >> 11u) & 2047u) << 5u, ((packed >> 22u) & 1023u) << 6u);
        unpacked = (unpacked << 16) >> 16;
        result = float4(float3(unpacked), scales[type]);
    }
    if (size < 4u) result.w = scales[type];
    return result / scales[type];
}
)";
    }
}
