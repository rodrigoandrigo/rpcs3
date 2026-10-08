#include "stdafx.h"
#include "vulkan_frame.h"
#include "Emu/RSX/D3D12/D3D12Presentation.h"
#include <cstring>

namespace
{
void checked(HRESULT result)
{
    if (FAILED(result)) fmt::throw_exception("Vulkan presentation HRESULT 0x%08x", static_cast<u32>(result));
}
class vulkan_frame final : public GSFrameBase
{
    bool visible = true;
public:
    void close() override { delete this; }
    void reset() override {}
    bool shown() override { return visible; }
    void hide() override { visible = false; }
    void show() override { visible = true; }
    void toggle_fullscreen() override {}
    void delete_context(draw_context_t) override {}
    draw_context_t make_context() override { return {}; }
    void set_current(draw_context_t) override {}
    void flip(draw_context_t, bool = false) override {}
    int client_width() override { return 1280; }
    int client_height() override { return 720; }
    f64 client_display_rate() override { return 60.; }
    bool has_alpha() override { return false; }
    display_handle_t handle() const override { return {}; }
    bool can_consume_frame() const override { return false; }
    void present_frame(std::vector<u8>&&, u32, u32, u32, bool) const override {}
    void take_screenshot(std::vector<u8>&&, u32, u32, bool) override {}
    void update_title(double = 0.) override {}
};
}

std::unique_ptr<GSFrameBase> make_vulkan_frame() { return std::make_unique<vulkan_frame>(); }
extern "C" void rpcs3_embedded_dzn_log(void*, const char* message)
{
    rsx_log.notice("Mesa DZN: %s", message ? message : "");
}

// Baseline presentation: completed Vulkan readback -> immutable host texture.
// No HWND/GDI and no reliance on cross-device resource sharing support.
void embedded_publish_bgra(const void* pixels, u32 width, u32 height)
{
    auto [device, queue] = d3d12::presentation().device_and_queue();
    using Microsoft::WRL::ComPtr;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> output, upload;
    checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&output)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 size{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = size; buffer.Height = 1; buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    u8* mapped{}; D3D12_RANGE empty{};
    checked(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)));
    for (u32 row = 0; row < height; ++row)
        std::memcpy(mapped + footprint.Offset + row * footprint.Footprint.RowPitch,
            static_cast<const u8*>(pixels) + static_cast<usz>(row) * width * 4, static_cast<usz>(width) * 4);
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    destination.pResource = output.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &barrier);
    checked(list->Close());
    checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    ID3D12CommandList* commands[] = {list.Get()};
    queue->ExecuteCommandLists(1, commands);
    checked(queue->Signal(fence.Get(), 1));
    // Keep allocator/upload alive until the queued copy completes. This runs
    // on the RSX worker, never the UI. Device loss follows the embedded error
    // boundary; a wall-clock timeout must not release in-flight resources.
    while (fence->GetCompletedValue() < 1)
    {
        checked(device->GetDeviceRemovedReason());
        Sleep(1);
    }
    checked(device->GetDeviceRemovedReason());
    d3d12::presentation().publish(output.Get());
}
