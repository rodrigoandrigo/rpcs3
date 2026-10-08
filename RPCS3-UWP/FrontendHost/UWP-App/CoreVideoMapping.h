#pragma once
#include <d3d12.h>

// Vulkan/DZN publishes BGRA; OpenGL and the legacy renderer publish RGBA.
// Typed SRVs perform the channel conversion, not a manual R/B swizzle.
namespace UwpImGuiFrontend::Sample
{
inline constexpr bool IsCoreVideoFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
}
// A PS3 display buffer is an opaque video plane, not a UI overlay. Preserve
// source RGB while ignoring its internal alpha only in the frontend SRV.
inline constexpr UINT CoreVideoComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
    D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
}
