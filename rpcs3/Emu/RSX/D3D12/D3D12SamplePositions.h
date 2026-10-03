#pragma once
#include "D3D12Readback.h"
#include <array>

namespace d3d12
{
// Coordinates relative to the pixel center, in D3D12 sixteenths.
// Keep sample indices in the same order as the 2x1 / 2x2 memory grid.
// Geometric candidate patterns, NOT an independently verified NV47/PS3
// sample-index fixture. Hardware equivalence must not be inferred from a
// successful D3D12 SetSamplePositions call.
enum class sample_pattern { center, diagonal, square, rotated };
inline std::array<D3D12_SAMPLE_POSITION, 4> sample_positions(sample_pattern pattern)
{
    switch (pattern)
    {
    case sample_pattern::center: return {{{0, 0}, {0, 0}, {0, 0}, {0, 0}}};
    case sample_pattern::diagonal: return {{{-4, -4}, {4, 4}, {0, 0}, {0, 0}}};
    case sample_pattern::square: return {{{-4, -4}, {4, -4}, {-4, 4}, {4, 4}}};
    case sample_pattern::rotated: return {{{-2, -6}, {6, -2}, {-6, 2}, {2, 6}}};
    }
    throw std::invalid_argument("Invalid RSX sample pattern");
}
inline void set_sample_positions(ID3D12Device* device, ID3D12GraphicsCommandList* commands, sample_pattern pattern)
{
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> extended;
    if (pattern == sample_pattern::center)
    {
        if (SUCCEEDED(commands->QueryInterface(IID_PPV_ARGS(extended.GetAddressOf())))) extended->SetSamplePositions(0, 0, nullptr);
        return;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 support{};
    readback_check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &support, sizeof(support)));
    if (support.ProgrammableSamplePositionsTier == D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED)
        throw std::runtime_error("The D3D12 device cannot program the requested RSX sample positions");
    readback_check(commands->QueryInterface(IID_PPV_ARGS(extended.GetAddressOf())));
    auto positions = sample_positions(pattern);
    extended->SetSamplePositions(pattern == sample_pattern::diagonal ? 2 : 4, 1, positions.data());
}
}
