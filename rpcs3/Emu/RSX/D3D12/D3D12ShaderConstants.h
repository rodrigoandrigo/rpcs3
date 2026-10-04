#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include "D3D12ShaderSplat.h"

namespace d3d12
{
    struct texture_constants
    {
        float scale[3]{};
        float bias[3]{};
        float clamp_min[2]{};
        float clamp_max[2]{};
        std::uint32_t remap{};
        std::uint32_t control{};
    };
    // Explicit cbuffer layout shared by both HLSL stages. TIU slots must not
    // be represented by an HLSL float3 struct, which would add padding.
    struct alignas(16) shader_constants
    {
        float scale_offset[16]{};
        std::int32_t clip_enabled[8]{};
        float clip_factor[8]{};
        float fog_param0{};
        float fog_param1{};
        std::uint32_t alpha_test{};
        float alpha_ref{};
        std::uint32_t alpha_func{};
        std::uint32_t fog_mode{};
        float wpos_scale{};
        float wpos_bias{};
        texture_constants textures[16]{};
        std::int32_t input_attributes[32]{};
        std::uint32_t vertex_index_offset{};
        std::uint32_t vertex_base_index{};
        std::uint32_t vertex_reserved[2]{};
        float vertex_texture_scale[4][4]{};
    };

    static_assert(sizeof(texture_constants) == 48);
    static_assert(offsetof(texture_constants, control) == 44);
    static_assert(offsetof(shader_constants, clip_enabled) == 64);
    static_assert(offsetof(shader_constants, clip_factor) == 96);
    static_assert(offsetof(shader_constants, fog_param0) == 128);
    static_assert(offsetof(shader_constants, textures) == 160);
    static_assert(offsetof(shader_constants, input_attributes) == 928);
    static_assert(offsetof(shader_constants, vertex_index_offset) == 1056);
    static_assert(offsetof(shader_constants, vertex_texture_scale) == 1072);
    static_assert(sizeof(shader_constants) == 1136);
    inline constexpr std::size_t shader_constants_allocation_size = 1280;

    inline void expand_clip_configuration(shader_constants& dst, std::uint32_t encoded) noexcept
    {
        for (unsigned i = 0; i < 6; ++i)
        {
            const auto operation = (encoded >> (i * 2)) & 3u;
            dst.clip_enabled[i] = operation == 0 || operation == 2;
            dst.clip_factor[i] = operation == 0 ? -1.f : operation == 2 ? 1.f : 0.f;
        }
    }

    inline void insert_shader_constants(std::ostream& output)
    {
        output << float_multiply_add;
        output << rsx_saturate;
        output << R"(#define floatBitsToUint asuint
#define uintBitsToFloat asfloat
cbuffer SCALE_OFFSET : register(b0)
{
    float4x4 scaleOffsetMat;
    int4 userClipEnabled[2];
    float4 userClipFactor[2];
    float fog_param0;
    float fog_param1;
    uint alpha_test;
    float alpha_ref;
    uint alpha_func;
    uint fog_mode;
    float wpos_scale;
    float wpos_bias;
    float4 texture_parameters[48];
    uint4 input_attributes_blob[8];
    uint vertex_index_offset;
    uint vertex_base_index;
    uint2 vertex_reserved;
    float4 vertex_texture_scale[4];
};
)";
    }
}
