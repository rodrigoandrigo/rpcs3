#include "Emu/RSX/D3D12/D3D12ShaderConstants.h"
#include "Emu/RSX/D3D12/D3D12VertexFetch.h"
#include "Emu/RSX/D3D12/D3D12VertexTextures.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <sstream>
#include <cstdio>

int main()
{
    d3d12::shader_constants constants{};
    for (unsigned plane = 0; plane < 6; ++plane)
    {
        for (unsigned operation = 0; operation < 3; ++operation)
        {
            constants = {};
            auto packed = (0x555u & ~(3u << (plane * 2))) | (operation << (plane * 2));
            d3d12::expand_clip_configuration(constants, packed);
            for (unsigned i = 0; i < 8; ++i)
            {
                const bool enabled = i == plane && operation != 1;
                const float factor = enabled ? (operation == 0 ? -1.f : 1.f) : 0.f;
                if (constants.clip_enabled[i] != static_cast<std::int32_t>(enabled) || constants.clip_factor[i] != factor) return 1;
            }
        }
    }

    std::ostringstream source;
    d3d12::insert_shader_constants(source);
    source << "float4 main() : SV_TARGET { return texture_parameters[47] + scaleOffsetMat[0] + userClipEnabled[0] + userClipFactor[0] + fog_param0 + fog_param1 + alpha_test + alpha_ref + alpha_func + fog_mode + wpos_scale + wpos_bias + input_attributes_blob[7] + vertex_index_offset + vertex_base_index + float4(vertex_reserved, 0., 0.); }";
    const auto hlsl = source.str();
    Microsoft::WRL::ComPtr<ID3DBlob> shader, diagnostics;
    auto result = D3DCompile(hlsl.data(), hlsl.size(), "rpcs3-constants-test.hlsl", nullptr, nullptr,
        "main", "ps_5_0", D3DCOMPILE_SKIP_OPTIMIZATION, 0, &shader, &diagnostics);
    if (FAILED(result))
    {
        std::printf("FAIL: D3DCompile 0x%08x: %s\n", static_cast<unsigned>(result),
            diagnostics ? static_cast<const char*>(diagnostics->GetBufferPointer()) : "no diagnostics");
        return 1;
    }
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    result = D3DReflect(shader->GetBufferPointer(), shader->GetBufferSize(),
        __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(reflection.GetAddressOf()));
    if (FAILED(result)) return 1;
    auto* buffer = reflection->GetConstantBufferByName("SCALE_OFFSET");
    D3D11_SHADER_BUFFER_DESC buffer_desc{};
    if (FAILED(buffer->GetDesc(&buffer_desc)) || buffer_desc.Size != sizeof(constants)) return 1;
    struct field { const char* name; unsigned offset; unsigned size; };
    const field fields[] = {
        {"scaleOffsetMat", 0, 64}, {"userClipEnabled", 64, 32}, {"userClipFactor", 96, 32},
        {"fog_param0", 128, 4}, {"fog_param1", 132, 4}, {"alpha_test", 136, 4}, {"alpha_ref", 140, 4},
        {"alpha_func", 144, 4}, {"fog_mode", 148, 4}, {"wpos_scale", 152, 4}, {"wpos_bias", 156, 4},
        {"texture_parameters", 160, 768}, {"input_attributes_blob", 928, 128},
        {"vertex_index_offset", 1056, 4}, {"vertex_base_index", 1060, 4}, {"vertex_reserved", 1064, 8},
        {"vertex_texture_scale", 1072, 64}
    };
    for (const auto& field : fields)
    {
        D3D11_SHADER_VARIABLE_DESC desc{};
        if (FAILED(buffer->GetVariableByName(field.name)->GetDesc(&desc)) ||
            desc.StartOffset != field.offset || desc.Size != field.size)
        {
            std::printf("FAIL: HLSL layout %s\n", field.name);
            return 1;
        }
    }
    std::ostringstream vertex_source;
    d3d12::insert_shader_constants(vertex_source);
    d3d12::insert_vertex_fetch(vertex_source);
    d3d12::insert_vertex_texture(vertex_source, 0, "1D");
    d3d12::insert_vertex_texture(vertex_source, 1, "2D");
    d3d12::insert_vertex_texture(vertex_source, 2, "3D");
    d3d12::insert_vertex_texture(vertex_source, 3, "Cube");
    vertex_source << "float4 main(uint vertex_id : SV_VertexID) : SV_POSITION { float4 p=read_location(5u, vertex_id); return vtex0_fetch(p)+vtex1_fetch(p)+vtex2_fetch(p)+vtex3_fetch(p); }";
    const auto vertex_hlsl = vertex_source.str();
    result = D3DCompile(vertex_hlsl.data(), vertex_hlsl.size(), "rpcs3-vertex-fetch-test.hlsl", nullptr, nullptr,
        "main", "vs_5_0", D3DCOMPILE_SKIP_OPTIMIZATION, 0, shader.ReleaseAndGetAddressOf(), diagnostics.ReleaseAndGetAddressOf());
    if (FAILED(result))
    {
        std::printf("FAIL: vertex HLSL 0x%08x: %s\n", static_cast<unsigned>(result),
            diagnostics ? static_cast<const char*>(diagnostics->GetBufferPointer()) : "no diagnostics");
        return 1;
    }
    std::puts("PASS: D3DCompile/reflection, 1136-byte cbuffer, vertex texture scales, raw vertex decoder and six packed clip planes");
    return 0;
}
