#pragma once
#include "D3D12MSAA.h"
#include <array>
#include <span>

namespace d3d12
{
// Resources and descriptors stay alive through the submission fence.
struct restored_surface
{
    Microsoft::WRL::ComPtr<ID3D12Resource> pixels, stencil, upload;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv, target;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
};

// Inverse of expand_samples: import the guest's expanded 2x1/2x2 grid,
// including per-sample depth and stencil. Uses no desktop-only API.
// Changes all graphics bindings; the caller must rebind its draw state.
inline std::shared_ptr<restored_surface> restore_surface(ID3D12Device* device,
    ID3D12GraphicsCommandList* commands, ID3D12Resource* destination,
    DXGI_FORMAT typed_format, std::span<const std::byte> pixels,
    std::span<const std::byte> stencil = {})
{
    using Microsoft::WRL::ComPtr;
    auto result = std::make_shared<restored_surface>();
    const auto desc = destination->GetDesc();
    const bool depth = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    const UINT count = desc.SampleDesc.Count;
    if (count != 1 && count != 2 && count != 4) throw std::invalid_argument("Unsupported surface sample count");
    const UINT sx = count == 1 ? 1 : 2, sy = count == 4 ? 2 : 1;
    const UINT width = UINT(desc.Width) * sx, height = desc.Height * sy;
    const bool has_stencil = !stencil.empty();
    if (has_stencil && (!depth || stencil.size() != std::size_t(width) * height))
        throw std::invalid_argument("Invalid surface stencil grid");
    D3D12_RESOURCE_DESC input{};
    input.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    input.Width = width; input.Height = height;
    input.DepthOrArraySize = input.MipLevels = 1;
    input.SampleDesc.Count = 1;
    input.Format = depth ? DXGI_FORMAT_R32_FLOAT : typed_format;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    readback_check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &input,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(result->pixels.GetAddressOf())));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2]{};
    UINT rows[2]{}; UINT64 row_bytes[2]{}, sizes[2]{};
    device->GetCopyableFootprints(&input, 0, 1, 0, &footprints[0], &rows[0], &row_bytes[0], &sizes[0]);
    if (pixels.size() != row_bytes[0] * rows[0]) throw std::invalid_argument("Invalid surface pixel grid");
    if (has_stencil)
    {
        input.Format = DXGI_FORMAT_R8_UINT;
        readback_check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &input,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(result->stencil.GetAddressOf())));
        device->GetCopyableFootprints(&input, 0, 1, (sizes[0] + 511) & ~511ull,
            &footprints[1], &rows[1], &row_bytes[1], &sizes[1]);
    }
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = has_stencil ? footprints[1].Offset + UINT64(footprints[1].Footprint.RowPitch) * rows[1]
        : UINT64(footprints[0].Footprint.RowPitch) * rows[0];
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    readback_check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(result->upload.GetAddressOf())));
    std::byte* mapped = nullptr; D3D12_RANGE no_read{};
    readback_check(result->upload->Map(0, &no_read, reinterpret_cast<void**>(&mapped)));
    for (UINT plane = 0; plane < (has_stencil ? 2u : 1u); ++plane)
    {
        const auto bytes = plane ? stencil : pixels;
        for (UINT y = 0; y < rows[plane]; ++y)
            std::memcpy(mapped + footprints[plane].Offset + std::size_t(y) * footprints[plane].Footprint.RowPitch,
                bytes.data() + std::size_t(y) * row_bytes[plane], std::size_t(row_bytes[plane]));
    }
    result->upload->Unmap(0, nullptr);
    for (UINT plane = 0; plane < (has_stencil ? 2u : 1u); ++plane)
    {
        auto* image = plane ? result->stencil.Get() : result->pixels.Get();
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = result->upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint = footprints[plane];
        D3D12_TEXTURE_COPY_LOCATION target{}; target.pResource = image;
        target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        commands->ResourceBarrier(1, &barrier);
    }
    D3D12_DESCRIPTOR_HEAP_DESC dh{};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; dh.NumDescriptors = 2;
    dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    readback_check(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(result->srv.GetAddressOf())));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    srv.Format = depth ? DXGI_FORMAT_R32_FLOAT : typed_format;
    auto handle = result->srv->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(result->pixels.Get(), &srv, handle);
    handle.ptr += device->GetDescriptorHandleIncrementSize(dh.Type);
    srv.Format = DXGI_FORMAT_R8_UINT;
    device->CreateShaderResourceView(has_stencil ? result->stencil.Get() : nullptr, &srv, handle);
    dh.Type = depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    dh.NumDescriptors = 1; dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    readback_check(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(result->target.GetAddressOf())));
    const auto target_handle = result->target->GetCPUDescriptorHandleForHeapStart();
    if (depth)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{}; dsv.Format = typed_format;
        dsv.ViewDimension = count == 1 ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DMS;
        device->CreateDepthStencilView(destination, &dsv, target_handle);
        // A newly allocated depth resource has no established sample pattern.
        // Establish it before any depth draw (optimized clear metadata alone
        // does not initialize the surface).
        commands->ClearDepthStencilView(target_handle, has_stencil
            ? D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL)
            : D3D12_CLEAR_FLAG_DEPTH, 0.f, 0, 0, nullptr);
    }
    else device->CreateRenderTargetView(destination, nullptr, target_handle);
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 2;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range}; parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 1; parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{}; signature.NumParameters = 2; signature.pParameters = parameters;
    ComPtr<ID3DBlob> blob, errors, vs, ps;
    readback_check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    readback_check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(result->root.GetAddressOf())));
    const std::string vertex = "float4 main(uint id:SV_VertexID):SV_POSITION {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
    const std::string grid = "uint2 p=uint2(pos.xy)*uint2(" + std::to_string(sx) + "," + std::to_string(sy) + ")+uint2(s%" + std::to_string(sx) + ",s/" + std::to_string(sx) + ");";
    const std::string fragment = depth
        ? "Texture2D<float> image:register(t0);Texture2D<uint> stencil:register(t1);cbuffer ref:register(b0){uint value;}float main(float4 pos:SV_POSITION,uint s:SV_SampleIndex):SV_DEPTH{" + grid + (has_stencil ? "if(stencil.Load(int3(p,0))!=value)discard;" : "") + "return image.Load(int3(p,0));}"
        : "Texture2D<float4> image:register(t0);float4 main(float4 pos:SV_POSITION,uint s:SV_SampleIndex):SV_TARGET{" + grid + "return image.Load(int3(p,0));}";
    readback_check(D3DCompile(vertex.data(), vertex.size(), "surface-restore-vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs, &errors));
    readback_check(D3DCompile(fragment.data(), fragment.size(), "surface-restore-ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps, errors.ReleaseAndGetAddressOf()));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = result->root.Get(); pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE; pso.RasterizerState.MultisampleEnable = count > 1;
    auto& blend = pso.BlendState.RenderTarget[0]; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE; blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    pso.DepthStencilState.DepthEnable = depth; pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.DepthStencilState.StencilEnable = has_stencil; pso.DepthStencilState.StencilReadMask = pso.DepthStencilState.StencilWriteMask = 255;
    pso.DepthStencilState.FrontFace = pso.DepthStencilState.BackFace =
        {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
    pso.SampleMask = UINT_MAX; pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.SampleDesc = desc.SampleDesc;
    if (depth) pso.DSVFormat = typed_format; else {pso.NumRenderTargets = 1; pso.RTVFormats[0] = typed_format;}
    readback_check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(result->pipeline.GetAddressOf())));
    ID3D12DescriptorHeap* descriptors = result->srv.Get(); commands->SetDescriptorHeaps(1, &descriptors);
    commands->SetGraphicsRootSignature(result->root.Get()); commands->SetPipelineState(result->pipeline.Get());
    commands->SetGraphicsRootDescriptorTable(0, result->srv->GetGPUDescriptorHandleForHeapStart());
    const D3D12_VIEWPORT viewport{0,0,float(desc.Width),float(desc.Height),0,1};
    const D3D12_RECT scissor{0,0,LONG(desc.Width),LONG(desc.Height)};
    commands->RSSetViewports(1,&viewport); commands->RSSetScissorRects(1,&scissor);
    commands->OMSetRenderTargets(depth ? 0 : 1, depth ? nullptr : &target_handle, TRUE, depth ? &target_handle : nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    std::array<bool,256> values{};
    for (auto value : stencil) values[std::to_integer<unsigned>(value)] = true;
    if (!has_stencil) values[0] = true;
    for (UINT value = 0; value < values.size(); ++value) if (values[value])
    {
        commands->OMSetStencilRef(value); commands->SetGraphicsRoot32BitConstant(1,value,0);
        commands->DrawInstanced(3,1,0,0);
    }
    return result;
}
}
