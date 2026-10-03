#pragma once
#include "D3D12Readback.h"
#include "D3D12SamplePositions.h"
#include <d3dcompiler.h>
#include <memory>
#include <unordered_map>

namespace d3d12
{
inline Microsoft::WRL::ComPtr<ID3D12Resource> resolve_color(ID3D12Device* device,
    ID3D12GraphicsCommandList* commands, ID3D12Resource* source, D3D12_RESOURCE_STATES state)
{
    auto desc = source->GetDesc();
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{desc.Format};
    readback_check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE))
        throw std::runtime_error("D3D12 presentation format cannot be resolved");
    desc.SampleDesc = {1, 0};
    desc.Alignment = 0;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    readback_check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(output.GetAddressOf())));
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE};
    commands->ResourceBarrier(1, &barrier);
    commands->ResolveSubresource(output.Get(), 0, source, 0, desc.Format);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(1, &barrier);
    barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_GENERIC_READ};
    commands->ResourceBarrier(1, &barrier);
    return output;
}
// Retain every descriptor and PSO until the command-list fence completes.
struct expanded_samples
{
    Microsoft::WRL::ComPtr<ID3D12Resource> image;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv, rtv;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
};

struct sample_pipeline_cache
{
    ID3D12Device* device = nullptr;
    struct shaders
    {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    };
    std::unordered_map<std::uint64_t, shaders> entries;
};

inline DXGI_FORMAT depth_sample_format(DXGI_FORMAT format, UINT plane)
{
    switch (format)
    {
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return plane ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return plane ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return format;
    }
}

// Expand samples instead of averaging them: RSX memory exposes 2x1 / 2x2
// sample grids. This also makes depth/stencil readable without a depth resolve.
// Keep lane order consistent with the Vulkan/OpenGL shared Program/MSAA
// resolve/unresolve shaders. This is not a hardware sample-position fixture.
// Graphics state is changed; the caller must bind its draw state afterwards.
inline std::shared_ptr<expanded_samples> expand_samples(ID3D12Device* device,
    ID3D12GraphicsCommandList* commands, ID3D12Resource* source,
    D3D12_RESOURCE_STATES state, bool depth = false, UINT plane = 0, sample_pipeline_cache* cache = nullptr,
    sample_pattern pattern = sample_pattern::center)
{
    using Microsoft::WRL::ComPtr;
    const auto desc = source->GetDesc();
    const UINT count = desc.SampleDesc.Count;
    if (count != 2 && count != 4) throw std::runtime_error("Unsupported D3D12 sample count");
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 || plane > 1)
        throw std::runtime_error("Unsupported D3D12 multisampled surface");
    const UINT sx = 2, sy = count / 2;
    auto result = std::make_shared<expanded_samples>();
    const auto format = depth ? (plane ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R32_FLOAT) : desc.Format;
    auto output = desc;
    output.Width *= sx;
    output.Height *= sy;
    output.Format = format;
    output.SampleDesc = {1, 0};
    output.Alignment = 0;
    output.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    readback_check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &output,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(result->image.GetAddressOf())));
    D3D12_DESCRIPTOR_HEAP_DESC dh{};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dh.NumDescriptors = 1;
    dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    readback_check(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(result->srv.GetAddressOf())));
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    readback_check(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(result->rtv.GetAddressOf())));
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = depth ? depth_sample_format(desc.Format, plane) : desc.Format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    view.Shader4ComponentMapping = plane
        ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1, 1, 1, 1) : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device->CreateShaderResourceView(source, &view, result->srv->GetCPUDescriptorHandleForHeapStart());
    const auto target = result->rtv->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(result->image.Get(), nullptr, target);
    const std::uint64_t key = (std::uint64_t(format) << 16) | (count << 1) | plane;
    if (cache)
    {
        if (cache->device && cache->device != device) throw std::runtime_error("MSAA shader cache belongs to another device");
        cache->device = device;
        if (const auto found = cache->entries.find(key); found != cache->entries.end())
        {
            result->root = found->second.root;
            result->pipeline = found->second.pipeline;
        }
    }
    if (!result->pipeline)
    {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &range};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 1;
        signature.pParameters = &parameter;
        ComPtr<ID3DBlob> blob, errors, vs, ps;
        readback_check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
        readback_check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(result->root.GetAddressOf())));
        const std::string vertex =
            "float4 main(uint id:SV_VertexID):SV_POSITION { float2 p=float2((id<<1)&2,id&2); return float4(p*float2(2,-2)+float2(-1,1),0,1); }";
        const std::string type = plane ? "uint4" : "float4";
        const std::string fragment = "Texture2DMS<" + type + "> image:register(t0); " + type +
            " main(float4 pos:SV_POSITION):SV_TARGET { uint2 p=uint2(pos.xy); uint s=(p.x%2)+(p.y%" +
            std::to_string(sy) + ")*2; return image.Load(p/uint2(2," + std::to_string(sy) + "),s); }";
        readback_check(D3DCompile(vertex.data(), vertex.size(), "sample-expand-vs", nullptr, nullptr,
            "main", "vs_5_0", 0, 0, &vs, &errors));
        readback_check(D3DCompile(fragment.data(), fragment.size(), "sample-expand-ps", nullptr, nullptr,
            "main", "ps_5_0", 0, 0, &ps, errors.ReleaseAndGetAddressOf()));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = result->root.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        auto& blend = pso.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.FrontFace = pso.DepthStencilState.BackFace =
            {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = format;
        pso.SampleDesc.Count = 1;
        readback_check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(result->pipeline.GetAddressOf())));
        if (cache) cache->entries.emplace(key, sample_pipeline_cache::shaders{result->root, result->pipeline});
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    // Depth transitions must use the pattern under which depth was written.
    set_sample_positions(device, commands, pattern);
    commands->ResourceBarrier(1, &barrier);
    set_sample_positions(device, commands, sample_pattern::center);
    ID3D12DescriptorHeap* descriptors = result->srv.Get();
    commands->SetDescriptorHeaps(1, &descriptors);
    commands->SetGraphicsRootSignature(result->root.Get());
    commands->SetPipelineState(result->pipeline.Get());
    commands->SetGraphicsRootDescriptorTable(0, result->srv->GetGPUDescriptorHandleForHeapStart());
    const D3D12_VIEWPORT viewport{0, 0, float(output.Width), float(output.Height), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(output.Width), LONG(output.Height)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(1, &target, TRUE, nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
    set_sample_positions(device, commands, pattern);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(1, &barrier);
    barrier.Transition = {result->image.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ};
    commands->ResourceBarrier(1, &barrier);
    return result;
}
}
