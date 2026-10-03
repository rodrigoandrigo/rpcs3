#pragma once
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace d3d12
{
inline void insert_vertex_texture(std::ostream& output, unsigned unit, std::string_view dimension)
{
    if (unit >= 4 || (dimension != "1D" && dimension != "2D" && dimension != "3D" && dimension != "Cube"))
        throw std::invalid_argument("Invalid D3D12 vertex texture declaration");
    const std::string name = "vtex" + std::to_string(unit);
    output << "Texture" << dimension << "<float4> " << name << " : register(t" << 16 + unit << ");\n";
    output << "SamplerState " << name << "_sampler : register(s" << unit << ");\n";
    const auto coordinate = dimension == "1D" ? "x" : dimension == "2D" ? "xy" : "xyz";
    output << "float4 " << name << "_fetch(float4 p) { return " << name << ".SampleLevel(" << name << "_sampler, p." << coordinate;
    if (dimension != "Cube") output << " * vertex_texture_scale[" << unit << "]." << coordinate;
    output << ", 0); }\n";
}
}
