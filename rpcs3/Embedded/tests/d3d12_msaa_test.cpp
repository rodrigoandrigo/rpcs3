#include "../../Emu/RSX/D3D12/D3D12MSAA.h"
#include "../../Emu/RSX/D3D12/D3D12SurfaceRestore.h"
#include "../../Emu/RSX/D3D12/D3D12VertexTextures.h"
#include <dxgi1_4.h>
#include <iostream>
#include <sstream>
#include <cmath>
#include <d3d12sdklayers.h>
using Microsoft::WRL::ComPtr;
using d3d12::readback_check;

// Exercise the real vertex texture declaration, a vertex-stage sample and
// per-sample PS execution. Different red values detect accidental averaging.
static std::shared_ptr<d3d12::expanded_samples> draw_sample_tags(ID3D12Device* device,
    ID3D12GraphicsCommandList* commands, ID3D12Resource* color, ID3D12Resource* texture,
    D3D12_CPU_DESCRIPTOR_HANDLE target, bool locations = false, float edge = .5f, bool horizontal = false)
{
    auto job = std::make_shared<d3d12::expanded_samples>();
    D3D12_DESCRIPTOR_HEAP_DESC dh{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    readback_check(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(job->srv.GetAddressOf())));
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device->CreateShaderResourceView(texture, &view, job->srv->GetCPUDescriptorHandleForHeapStart());
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 16;
    D3D12_ROOT_PARAMETER root{};
    root.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root.DescriptorTable = {1, &range};
    root.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC signature{1, &root, 1, &sampler};
    ComPtr<ID3DBlob> blob, errors, vs, ps;
    readback_check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    readback_check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(job->root.GetAddressOf())));
    std::ostringstream source;
    source << "static const float4 vertex_texture_scale[4]={float4(1,1,1,1),float4(1,1,1,1),float4(1,1,1,1),float4(1,1,1,1)};\n";
    d3d12::insert_vertex_texture(source, 0, "2D");
    source << "struct V {float4 p:SV_POSITION; float4 c:COLOR0;}; V main(uint id:SV_VertexID) {V o; float2 p=float2((id<<1)&2,id&2);o.p=float4(p*float2(2,-2)+float2(-1,1),0,1);o.c="
        << (locations ? "float4(p,0,1)" : "vtex0_fetch(float4(.5,.5,0,0))") << ";return o;}";
    if (locations)
    {
        source.str(""); source.clear();
        source << "struct V{float4 p:SV_POSITION;float4 c:COLOR0;};V main(uint id:SV_VertexID){V o;static const float2 q[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};float2 p=q[id]*float2("
            << (horizontal ? 1.f : edge/17.f) << "," << (horizontal ? edge/3.f : 1.f)
            << ");o.p=float4(p*float2(2,-2)+float2(-1,1),0,1);o.c=1;return o;}";
    }
    const auto vertex = source.str();
    const char* fragment = locations
        ? "float4 main():SV_TARGET {return 1;}"
        : "float4 main(float4 pos:SV_POSITION,float4 color:COLOR0,uint sample:SV_SampleIndex):SV_TARGET {return float4(sample/3.,color.b,color.r,1);}";
    readback_check(D3DCompile(vertex.data(), vertex.size(), "vertex-texture-gpu", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs, &errors));
    readback_check(D3DCompile(fragment, std::strlen(fragment), "sample-tags", nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps, errors.ReleaseAndGetAddressOf()));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = job->root.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.MultisampleEnable = TRUE;
    auto& blend = pso.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.DepthStencilState.FrontFace = pso.DepthStencilState.BackFace =
        {D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc = color->GetDesc().SampleDesc;
    readback_check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(job->pipeline.GetAddressOf())));
    ID3D12DescriptorHeap* heaps = job->srv.Get();
    commands->SetDescriptorHeaps(1, &heaps);
    commands->SetGraphicsRootSignature(job->root.Get());
    commands->SetPipelineState(job->pipeline.Get());
    commands->SetGraphicsRootDescriptorTable(0, job->srv->GetGPUDescriptorHandleForHeapStart());
    const D3D12_VIEWPORT viewport{0,0,17,3,0,1};
    const D3D12_RECT scissor{0,0,17,3};
    commands->RSSetViewports(1,&viewport);
    commands->RSSetScissorRects(1,&scissor);
    commands->OMSetRenderTargets(1,&target,TRUE,nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(locations ? 6 : 3,1,0,0);
    return job;
}

int main()
{
    ComPtr<ID3D12Device> device;
    try
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf())))) debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        readback_check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf())));
        ComPtr<IDXGIAdapter> warp;
        readback_check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.GetAddressOf())));
        readback_check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())));
        D3D12_COMMAND_QUEUE_DESC q{};
        ComPtr<ID3D12CommandQueue> queue;
        readback_check(device->CreateCommandQueue(&q, IID_PPV_ARGS(queue.GetAddressOf())));
        ComPtr<ID3D12CommandAllocator> allocator;
        readback_check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(allocator.GetAddressOf())));
        ComPtr<ID3D12GraphicsCommandList> commands;
        readback_check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(commands.GetAddressOf())));
        D3D12_FEATURE_DATA_D3D12_OPTIONS2 positions_support{};
        readback_check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2,&positions_support,sizeof(positions_support)));
        const bool programmable = positions_support.ProgrammableSamplePositionsTier != D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
        std::cout << "WARP programmable sample positions tier=" << positions_support.ProgrammableSamplePositionsTier << "\n";
        if (!programmable)
        {
            bool rejected=false;
            try { d3d12::set_sample_positions(device.Get(),commands.Get(),d3d12::sample_pattern::square); }
            catch (const std::runtime_error&) {rejected=true;}
            if (!rejected) throw std::runtime_error("Unsupported sample positions silently approximated");
            std::cout << "PASS: unsupported programmable positions rejected; geometry execution SKIPPED\n";
        }
        d3d12::sample_pipeline_cache cache;
        for (auto tested_pattern : {d3d12::sample_pattern::diagonal,d3d12::sample_pattern::square,d3d12::sample_pattern::rotated})
        for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT,
            DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT})
        {
            const UINT samples = tested_pattern == d3d12::sample_pattern::diagonal ? 2 : 4;
            const bool depth = format != DXGI_FORMAT_R8G8B8A8_UNORM;
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{format, samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
            readback_check(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality)));
            if (!quality.NumQualityLevels) throw std::runtime_error("WARP sample count unsupported");
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = 17;
            desc.Height = 3;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.Format = format;
            if (format == DXGI_FORMAT_D16_UNORM) desc.Format = DXGI_FORMAT_R16_TYPELESS;
            if (format == DXGI_FORMAT_D24_UNORM_S8_UINT) desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
            if (format == DXGI_FORMAT_D32_FLOAT) desc.Format = DXGI_FORMAT_R32_TYPELESS;
            if (format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
            desc.SampleDesc = {samples, 0};
            desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear{};
            clear.Format = format;
            clear.DepthStencil.Depth = 0.25f;
            clear.DepthStencil.Stencil = 0x5a;
            if (!depth) { clear.Color[0] = clear.Color[3] = 1; clear.Color[1] = 0; clear.Color[2] = 0.5f; }
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            const auto state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
            const auto pattern = programmable ? tested_pattern : d3d12::sample_pattern::center;
            d3d12::set_sample_positions(device.Get(),commands.Get(),pattern);
            ComPtr<ID3D12Resource> surface;
            readback_check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, &clear,
                IID_PPV_ARGS(surface.GetAddressOf())));
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = 1;
            ComPtr<ID3D12DescriptorHeap> descriptors;
            readback_check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(descriptors.GetAddressOf())));
            const auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
            const bool stencil = format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            if (depth)
            {
                D3D12_DEPTH_STENCIL_VIEW_DESC ds{};
                ds.Format = format;
                ds.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
                device->CreateDepthStencilView(surface.Get(), &ds, handle);
                commands->ClearDepthStencilView(handle, stencil ? D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL)
                    : D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0x5a, 0, nullptr);
            }
            else
            {
                device->CreateRenderTargetView(surface.Get(), nullptr, handle);
                commands->ClearRenderTargetView(handle, clear.Color, 0, nullptr);
            }
            auto expanded = d3d12::expand_samples(device.Get(), commands.Get(), surface.Get(), state, depth, 0, &cache,pattern);
            auto data = d3d12::download_plane(device.Get(), queue.Get(), commands.Get(), expanded->image.Get(), D3D12_RESOURCE_STATE_GENERIC_READ, 0);
            if (data.width != 34 || data.height != 3 * samples / 2 || data.bytes.size() != 17 * 3 * samples * 4)
                throw std::runtime_error("Sample expansion dimensions incorrect");
            for (size_t i = 0; i < data.bytes.size(); i += 4)
            {
                if (depth)
                {
                    float z;
                    std::memcpy(&z, data.bytes.data() + i, 4);
                    if (std::abs(z - 0.25f) > 0.00002f) throw std::runtime_error("Expanded depth sample incorrect");
                }
                else if (data.bytes[i] != std::byte{255} || data.bytes[i+1] != std::byte{0} ||
                    data.bytes[i+2] != std::byte{128} || data.bytes[i+3] != std::byte{255})
                    throw std::runtime_error("Expanded color sample incorrect");
            }
            readback_check(allocator->Reset());
            readback_check(commands->Reset(allocator.Get(), nullptr));
            if (stencil)
            {
                auto extracted = d3d12::expand_samples(device.Get(), commands.Get(), surface.Get(), state, true, 1, &cache,pattern);
                auto s = d3d12::download_plane(device.Get(), queue.Get(), commands.Get(), extracted->image.Get(), D3D12_RESOURCE_STATE_GENERIC_READ, 0);
                for (auto value : s.bytes) if (value != std::byte{0x5a}) throw std::runtime_error("Expanded stencil sample incorrect");
                readback_check(allocator->Reset());
                readback_check(commands->Reset(allocator.Get(), nullptr));
            }
            if (!depth)
            {
                auto resolved = d3d12::resolve_color(device.Get(), commands.Get(), surface.Get(), state);
                auto resolved_data = d3d12::download_plane(device.Get(), queue.Get(), commands.Get(), resolved.Get(), D3D12_RESOURCE_STATE_GENERIC_READ, 0);
                if (resolved_data.width != 17 || resolved_data.height != 3 || resolved_data.bytes[0] != std::byte{255})
                    throw std::runtime_error("Presentation resolve incorrect");
                readback_check(allocator->Reset());
                readback_check(commands->Reset(allocator.Get(), nullptr));
                d3d12::set_sample_positions(device.Get(),commands.Get(),pattern);
                auto tags = draw_sample_tags(device.Get(), commands.Get(), surface.Get(), resolved.Get(), handle);
                auto tagged = d3d12::expand_samples(device.Get(), commands.Get(), surface.Get(), state, false, 0, &cache,pattern);
                auto tagged_data = d3d12::download_plane(device.Get(), queue.Get(), commands.Get(), tagged->image.Get(), D3D12_RESOURCE_STATE_GENERIC_READ, 0);
                for (UINT y=0; y<tagged_data.height; ++y)
                for (UINT x=0; x<tagged_data.width; ++x)
                {
                    const UINT sample = x%2 + (y%(samples/2))*2;
                    const size_t pixel = y*tagged_data.row_bytes+x*4;
                    if (tagged_data.bytes[pixel] != std::byte(sample*85) ||
                        tagged_data.bytes[pixel+1] != std::byte{128} || tagged_data.bytes[pixel+2] != std::byte{255})
                        throw std::runtime_error("Vertex texture/sample identity check failed");
                }
                readback_check(allocator->Reset());
                readback_check(commands->Reset(allocator.Get(), nullptr));
            }
                if (programmable && !depth)
                {
                  for (bool horizontal : {false,true})
                  for (float edge : {.1875f,.5f,.8125f})
                  {
                    d3d12::set_sample_positions(device.Get(),commands.Get(),pattern);
                    auto location_input = d3d12::resolve_color(device.Get(),commands.Get(),surface.Get(),state);
                    const float zero[4]{};
                    commands->ClearRenderTargetView(handle,zero,0,nullptr);
                    auto locations = draw_sample_tags(device.Get(),commands.Get(),surface.Get(),location_input.Get(),handle,true,edge,horizontal);
                    auto location_image = d3d12::expand_samples(device.Get(),commands.Get(),surface.Get(),state,false,0,&cache,pattern);
                    auto location_data = d3d12::download_plane(device.Get(),queue.Get(),commands.Get(),location_image->image.Get(),D3D12_RESOURCE_STATE_GENERIC_READ,0);
                    const auto positions = d3d12::sample_positions(pattern);
                    for (UINT y=0;y<location_data.height;++y)
                    for (UINT x=0;x<location_data.width;++x)
                    {
                        const UINT s=x%2+(y%(samples/2))*2;
                        const size_t offset=y*location_data.row_bytes+x*4;
                        const float coordinate = horizontal ? float(y/(samples/2))+(positions[s].Y+8)/16.f
                            : float(x/2)+(positions[s].X+8)/16.f;
                        const int wanted = coordinate < edge ? 255 : 0;
                        if (std::to_integer<int>(location_data.bytes[offset])!=wanted)
                            throw std::runtime_error("Programmed sample position/edge coverage mismatch");
                    }
                    readback_check(allocator->Reset());readback_check(commands->Reset(allocator.Get(),nullptr));
                  }
                    std::cout << "PASS: programmed sample edge coverage pattern=" << unsigned(pattern) << "\n";
                }
            // Simulate CPU writes to every sample after an implicit readback.
            std::vector<std::byte> cpu_pixels(data.bytes.size()), cpu_stencil;
            if (stencil) cpu_stencil.resize(data.width * data.height);
            for (UINT y = 0; y < data.height; ++y)
            for (UINT x = 0; x < data.width; ++x)
            {
                const size_t i = y * data.width + x;
                if (depth)
                {
                    const float z = float(1 + i % 7) / 8.f;
                    std::memcpy(cpu_pixels.data() + i * 4, &z, 4);
                }
                else
                {
                    cpu_pixels[i*4] = std::byte(i % 256);
                    cpu_pixels[i*4+1] = std::byte{117}; cpu_pixels[i*4+2] = std::byte{233}; cpu_pixels[i*4+3] = std::byte{255};
                }
                if (stencil) cpu_stencil[i] = std::byte(i % 11);
            }
            d3d12::set_sample_positions(device.Get(),commands.Get(),pattern);
            auto restored = d3d12::restore_surface(device.Get(), commands.Get(), surface.Get(), format, cpu_pixels, cpu_stencil);
            auto roundtrip = d3d12::expand_samples(device.Get(), commands.Get(), surface.Get(), state, depth, 0, &cache,pattern);
            auto actual = d3d12::download_plane(device.Get(), queue.Get(), commands.Get(), roundtrip->image.Get(), D3D12_RESOURCE_STATE_GENERIC_READ, 0);
            if (depth)
            {
                for (size_t i=0; i<cpu_pixels.size(); i+=4)
                {
                    float wanted, got; std::memcpy(&wanted,cpu_pixels.data()+i,4); std::memcpy(&got,actual.bytes.data()+i,4);
                    if (std::abs(wanted-got)>0.00002f) throw std::runtime_error("CPU depth restoration lost a sample");
                }
            }
            else if (actual.bytes != cpu_pixels) throw std::runtime_error("CPU color restoration lost a sample");
            readback_check(allocator->Reset()); readback_check(commands->Reset(allocator.Get(),nullptr));
            if (stencil)
            {
                auto s_image = d3d12::expand_samples(device.Get(),commands.Get(),surface.Get(),state,true,1,&cache,pattern);
                auto s = d3d12::download_plane(device.Get(),queue.Get(),commands.Get(),s_image->image.Get(),D3D12_RESOURCE_STATE_GENERIC_READ,0);
                if (s.bytes != cpu_stencil) throw std::runtime_error("CPU stencil restoration lost a sample");
                readback_check(allocator->Reset()); readback_check(commands->Reset(allocator.Get(),nullptr));
            }
            std::cout << "PASS: WARP samples=" << samples << " format=" << format << " sample-grid/depth/stencil/resolve/CPU-restore\n";
        }
        readback_check(commands->Close());
        if (cache.entries.size() != 6) throw std::runtime_error("MSAA pipelines were not shared by compatible formats");
        ComPtr<ID3D12InfoQueue> info;
        if (SUCCEEDED(device.As(&info)))
        {
            for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
            {
                SIZE_T size = 0;
                info->GetMessage(i, nullptr, &size);
                std::vector<std::byte> bytes(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                readback_check(info->GetMessage(i, message, &size));
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
            }
        }
        std::cout << "PASS: vertex-stage texture sampling, distinct sample identities, six reused pipelines, no debug-layer errors\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        ComPtr<ID3D12InfoQueue> info;
        if (device && SUCCEEDED(device.As(&info)))
        {
            for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
            {
                SIZE_T size = 0;
                info->GetMessage(i, nullptr, &size);
                std::vector<std::byte> bytes(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                if (SUCCEEDED(info->GetMessage(i, message, &size))) std::cerr << message->pDescription << "\n";
            }
        }
        return 1;
    }
}
