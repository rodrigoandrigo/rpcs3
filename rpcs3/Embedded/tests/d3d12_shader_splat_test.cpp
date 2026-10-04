#include "../../Emu/RSX/D3D12/D3D12ShaderSplat.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv)
{
	const std::string input = "float4(0.) int4(0) float2(1.0f) bool3(false) float4(v) float4(v.xyz, 1.) myfloat4(0.)";
	const std::string expected = "float4(0., 0., 0., 0.) int4(0, 0, 0, 0) float2(1.0f, 1.0f) bool3(false, false, false) float4(v) float4(v.xyz, 1.) myfloat4(0.)";
	if (d3d12::expand_literal_splats(input) != expected || d3d12::expand_literal_splats(expected) != expected)
		return 1;
	if (argc != 2 && argc != 3) return 2;
	const bool fragment = argc == 3 && std::string(argv[2]) == "ps_5_0";
	const char* profile = fragment ? "ps_5_0" : "vs_5_0";
	std::ifstream file(argv[1], std::ios::binary);
	if (!file) return 3;
	const std::string original{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	const auto corrected = (fragment ? std::string(d3d12::rsx_saturate) : std::string(d3d12::vertex_constant_fetch) + std::string(d3d12::float_multiply_add)) + d3d12::expand_literal_splats(original);
	ID3DBlob* bytecode = nullptr;
	ID3DBlob* errors = nullptr;
	HRESULT hr = D3DCompile(original.data(), original.size(), "captured.hlsl", nullptr, nullptr, "main", profile, 0, 0, &bytecode, &errors);
	const std::string diagnostics = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "";
	const bool reproduced = FAILED(hr) && diagnostics.find(fragment ? "X3004" : "X3014") != std::string::npos && (!fragment || diagnostics.find("_saturate") != std::string::npos);
	if (bytecode) bytecode->Release();
	if (errors) errors->Release();
	if (!reproduced) return 4;
	bytecode = nullptr; errors = nullptr;
	hr = D3DCompile(corrected.data(), corrected.size(), "corrected.hlsl", nullptr, nullptr, "main", profile, 0, 0, &bytecode, &errors);
	if (FAILED(hr) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
	if (bytecode) bytecode->Release();
	if (errors) errors->Release();
	if (FAILED(hr)) return 5;
	std::cout << "PASS: literal splats, idempotence, captured error reproduced, corrected " << profile << " compiles\n";
}
