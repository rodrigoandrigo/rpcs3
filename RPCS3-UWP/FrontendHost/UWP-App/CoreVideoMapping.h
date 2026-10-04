#pragma once
#include <d3d12.h>

namespace UwpImGuiFrontend::Sample
{
// A PS3 display buffer is an opaque video plane, not a UI overlay. Preserve
// source RGB while ignoring its internal alpha only in the frontend SRV.
inline constexpr UINT CoreVideoComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
    D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
}
