#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace d3d12
{
// RSX Z16F is unsigned e4m12, not IEEE half. Matches the current RSX/VK codec.
inline std::uint16_t pack_depth_e4m12(std::uint32_t bits)
{
    return static_cast<std::uint16_t>((bits - (120u << 23)) >> 11);
}
struct readback_plane
{
    std::vector<std::byte> bytes;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0;
    std::size_t row_bytes = 0;
};

inline void readback_check(HRESULT result)
{
    if (FAILED(result)) throw std::runtime_error("D3D12 readback failed: " + std::to_string(static_cast<unsigned long>(result)));
}

// Flushes the supplied OPEN command list, including any preceding draws. The
// caller must replace/reset that list only after this function returns.
inline readback_plane download_plane(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* commands, ID3D12Resource* source,
    D3D12_RESOURCE_STATES state, UINT subresource)
{
    using Microsoft::WRL::ComPtr;
    const auto desc = source->GetDesc();
    if (desc.SampleDesc.Count != 1) throw std::runtime_error("Readback requires a single-sample surface");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0, size = 0;
    device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &rows, &row_bytes, &size);
    if (!size || size == UINT64_MAX || size > SIZE_MAX || row_bytes > SIZE_MAX / (std::max)(1u, rows))
        throw std::runtime_error("Invalid D3D12 readback footprint");
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = size;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    readback_check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(staging.GetAddressOf())));
    ComPtr<ID3D12Fence> fence;
    readback_check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
    struct event_owner
    {
        HANDLE handle = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
        ~event_owner() { if (handle) CloseHandle(handle); }
    } event;
    if (!event.handle) readback_check(HRESULT_FROM_WIN32(GetLastError()));
    // Set up the completion event before submitting any work.
    readback_check(fence->SetEventOnCompletion(1, event.handle));
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {source, subresource, state, D3D12_RESOURCE_STATE_COPY_SOURCE};
    commands->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = staging.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = source;
    origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    origin.SubresourceIndex = subresource;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(1, &barrier);
    readback_check(commands->Close());
    ID3D12CommandList* list = commands;
    queue->ExecuteCommandLists(1, &list);
    readback_check(queue->Signal(fence.Get(), 1));
    // Do not release staging/allocators while healthy-device GPU work is pending.
    // Periodic checks retain the actual device-removal HRESULT rather than hanging
    // forever on an event that a removed device will never signal.
    while (fence->GetCompletedValue() < 1)
    {
        const DWORD wait = WaitForSingleObjectEx(event.handle, 1000, FALSE);
        if (wait == WAIT_FAILED) readback_check(HRESULT_FROM_WIN32(GetLastError()));
        readback_check(device->GetDeviceRemovedReason());
    }
    readback_check(device->GetDeviceRemovedReason());
    readback_plane result;
    result.format = footprint.Footprint.Format;
    result.width = footprint.Footprint.Width;
    result.height = rows;
    result.row_bytes = static_cast<std::size_t>(row_bytes);
    result.bytes.resize(result.row_bytes * rows);
    const D3D12_RANGE range{0, static_cast<SIZE_T>(size)};
    void* mapped = nullptr;
    readback_check(staging->Map(0, &range, &mapped));
    for (UINT row = 0; row < rows; ++row)
        std::memcpy(result.bytes.data() + row * result.row_bytes,
            static_cast<const std::byte*>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch,
            result.row_bytes);
    const D3D12_RANGE no_writes{0, 0};
    staging->Unmap(0, &no_writes);
    return result;
}
}
