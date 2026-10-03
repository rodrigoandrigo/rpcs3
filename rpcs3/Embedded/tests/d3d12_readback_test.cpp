#include "../../Emu/RSX/D3D12/D3D12Readback.h"
#include "../../Emu/RSX/D3D12/D3D12Presentation.h"
#include <dxgi1_4.h>
#include <iostream>
#include <cmath>

using Microsoft::WRL::ComPtr;
using d3d12::readback_check;

int main()
{
    try
    {
        ComPtr<IDXGIFactory4> factory;
        readback_check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf())));
        ComPtr<IDXGIAdapter> warp;
        readback_check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.GetAddressOf())));
        ComPtr<ID3D12Device> device;
        readback_check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())));
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        readback_check(device->CreateCommandQueue(&q, IID_PPV_ARGS(queue.GetAddressOf())));
        ComPtr<ID3D12CommandAllocator> allocator;
        readback_check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(allocator.GetAddressOf())));
        ComPtr<ID3D12GraphicsCommandList> commands;
        readback_check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(commands.GetAddressOf())));
        for (const auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D16_UNORM,
            DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT})
        {
            const bool depth = format != DXGI_FORMAT_R8G8B8A8_UNORM;
            const auto state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = 17;
            desc.Height = 3;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_CLEAR_VALUE clear{};
            clear.Format = format;
            if (depth) { clear.DepthStencil.Depth = 0.25f; clear.DepthStencil.Stencil = 0x5a; }
            else { clear.Color[0] = clear.Color[3] = 1; clear.Color[2] = 0.5f; }
            ComPtr<ID3D12Resource> surface;
            readback_check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                &clear, IID_PPV_ARGS(surface.GetAddressOf())));
            ComPtr<ID3D12DescriptorHeap> descriptors;
            D3D12_DESCRIPTOR_HEAP_DESC dh{};
            dh.Type = depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            dh.NumDescriptors = 1;
            readback_check(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(descriptors.GetAddressOf())));
            const auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
            if (depth)
            {
                device->CreateDepthStencilView(surface.Get(), nullptr, handle);
                commands->ClearDepthStencilView(handle, (format == DXGI_FORMAT_D16_UNORM || format == DXGI_FORMAT_D32_FLOAT) ? D3D12_CLEAR_FLAG_DEPTH
                    : D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL), 0.25f, 0x5a, 0, nullptr);
            }
            else
            {
                device->CreateRenderTargetView(surface.Get(), nullptr, handle);
                commands->ClearRenderTargetView(handle, clear.Color, 0, nullptr);
            }
            auto data = d3d12::download_plane(device.Get(), queue.Get(), commands.Get(), surface.Get(), state, 0);
            if (data.height != 3 || data.width != 17 || data.bytes.size() != data.row_bytes * 3)
                throw std::runtime_error("Incorrect tight readback dimensions");
            const auto stride = data.row_bytes / 17;
            for (std::size_t i = 0; i < 51; ++i)
            {
                if (!depth)
                {
                    const auto* p = data.bytes.data() + i * stride;
                    if (p[0] != std::byte{255} || p[1] != std::byte{0} || p[2] != std::byte{128} || p[3] != std::byte{255})
                        throw std::runtime_error("Incorrect RGBA readback or row padding");
                }
                else if (format == DXGI_FORMAT_D16_UNORM)
                {
                    unsigned short value;
                    std::memcpy(&value, data.bytes.data() + i * stride, 2);
                    if (value < 16383 || value > 16384) throw std::runtime_error("Incorrect D16 depth");
                }
                else if (format == DXGI_FORMAT_D24_UNORM_S8_UINT)
                {
                    unsigned value;
                    std::memcpy(&value, data.bytes.data() + i * stride, 4);
                    value &= 0xffffff;
                    if (value < 4194303 || value > 4194304) throw std::runtime_error("Incorrect D24 depth");
                }
                else
                {
                    float value;
                    std::memcpy(&value, data.bytes.data() + i * stride, 4);
                    if (value != 0.25f) throw std::runtime_error("Incorrect D32 depth");
                }
            }
            readback_check(allocator->Reset());
            readback_check(commands->Reset(allocator.Get(), nullptr));
            if (format == DXGI_FORMAT_D32_FLOAT)
            {
                unsigned bits;
                std::memcpy(&bits, data.bytes.data(), 4);
                if (d3d12::pack_depth_e4m12(bits) != 0x5000) throw std::runtime_error("Incorrect RSX e4m12 depth conversion");
            }
            if (depth && format != DXGI_FORMAT_D16_UNORM && format != DXGI_FORMAT_D32_FLOAT)
            {
                auto stencil = d3d12::download_plane(device.Get(), queue.Get(), commands.Get(), surface.Get(), state, 1);
                const auto ss = stencil.row_bytes / 17;
                const auto component = ss == 1 ? 0 : ss == 4 ? 3 : 4;
                if (component >= ss) throw std::runtime_error("Unknown stencil footprint");
                for (std::size_t i = 0; i < 51; ++i)
                    if (stencil.bytes[i * ss + component] != std::byte{0x5a}) throw std::runtime_error("Incorrect stencil plane");
                readback_check(allocator->Reset());
                readback_check(commands->Reset(allocator.Get(), nullptr));
                std::cout << "stencil stride=" << ss << '\n';
            }
            std::cout << "PASS: WARP format=" << format << " depth/color stride=" << stride << ", 17x3 padded rows\n";
            if (!depth)
            {
                d3d12::presentation_context presentation;
                presentation.attach(device.Get(), queue.Get());
                bool duplicate_rejected = false;
                try { presentation.attach(device.Get(), queue.Get()); }
                catch (const std::logic_error&) { duplicate_rejected = true; }
                if (!duplicate_rejected) throw std::runtime_error("Duplicate graphics attachment accepted");
                presentation.publish(surface.Get());
                std::uint64_t serial = 0;
                auto acquired = presentation.acquire(serial);
                if (!acquired || acquired.Get() != surface.Get() || serial != 1)
                    throw std::runtime_error("Incorrect shared frame publication");
                surface.Reset();
                presentation.detach();
                if (presentation.acquire(serial)) throw std::runtime_error("Detached frame is still published");
                if (acquired->GetDesc().Width != 17) throw std::runtime_error("Acquired frame reference was not retained");
                std::cout << "PASS: shared device/queue attachment, immutable frame acquisition and retained COM lifetime\n";
            }
        }
        readback_check(commands->Close());
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
