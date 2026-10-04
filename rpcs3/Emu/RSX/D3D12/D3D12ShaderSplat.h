#pragma once
#include <string>
#include <string_view>

namespace d3d12
{
// D3D12 uploads the current draw's constants directly into vc (no indirection).
inline constexpr std::string_view vertex_constant_fetch = "#define _fetch_constant(x) vc[x]\n";
// Shader Model 5 fma is double-only; RSX operands are single precision.
inline constexpr std::string_view float_multiply_add = "#define fma mad\n";
// Match the shared RSX helper's clamp expression for scalar/vector operands.
inline constexpr std::string_view rsx_saturate = "#define _saturate(x) clamp((x), 0.0f, 1.0f)\n";

// Shared GLSL decompilers emit scalar zero/one constructors. HLSL requires
// explicit vector components. Only literal splats are expanded: expressions
// may already be vectors and must retain their original evaluation semantics.
inline std::string expand_literal_splats(std::string source)
{
	for (const std::string_view type : {"float", "int", "uint", "bool"})
	{
		for (unsigned width = 2; width <= 4; ++width)
		{
			for (const std::string_view value : {"0", "0.", "0.0", "0.0f", "1", "1.", "1.0", "1.0f", "false", "true"})
			{
				const std::string prefix = std::string(type) + std::to_string(width);
				const std::string original = prefix + "(" + std::string(value) + ")";
				std::string replacement = prefix + "(";
				for (unsigned component = 0; component < width; ++component)
				{
					if (component) replacement += ", ";
					replacement += value;
				}
				replacement += ")";
				size_t position = 0;
				while ((position = source.find(original, position)) != std::string::npos)
				{
					const char previous = position ? source[position - 1] : '\0';
					const bool identifier = (previous >= 'a' && previous <= 'z') ||
						(previous >= 'A' && previous <= 'Z') || (previous >= '0' && previous <= '9') || previous == '_';
					if (!identifier)
					{
						source.replace(position, original.size(), replacement);
						position += replacement.size();
					}
					else position += original.size();
				}
			}
		}
	}
	return source;
}
}
