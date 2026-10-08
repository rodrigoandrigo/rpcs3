#include "../../../RPCS3-UWP/FrontendHost/UWP-App/CoreVideoMapping.h"
#include <array>
#include <iostream>

int main()
{
    using UwpImGuiFrontend::Sample::IsCoreVideoFormat;
    static_assert(IsCoreVideoFormat(DXGI_FORMAT_R8G8B8A8_UNORM));
    static_assert(IsCoreVideoFormat(DXGI_FORMAT_B8G8R8A8_UNORM));
    static_assert(!IsCoreVideoFormat(DXGI_FORMAT_R16G16B16A16_FLOAT));
    static_assert(!IsCoreVideoFormat(DXGI_FORMAT_UNKNOWN));
    constexpr auto mapping = UwpImGuiFrontend::Sample::CoreVideoComponentMapping;
    for (unsigned channel = 0; channel < 3; ++channel)
        if (D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(channel, mapping) != channel) return 1;
    if (D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(3, mapping) != D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1) return 2;
    for (const float alpha : {0.f, 0.5f, 1.f})
    {
        const std::array<float, 4> source{0.25f, 0.5f, 0.75f, alpha};
        std::array<float, 4> sampled{};
        for (unsigned channel = 0; channel < 4; ++channel)
        {
            const auto selector = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(channel, mapping);
            sampled[channel] = selector < 4 ? source[selector] : selector == D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1 ? 1.f : 0.f;
        }
        if (sampled[0] != source[0] || sampled[1] != source[1] || sampled[2] != source[2] || sampled[3] != 1.f || source[3] != alpha) return 3;
    }
    std::cout << "PASS: frontend video mapping preserves RGB/source alpha and samples opaque alpha\n";
}
