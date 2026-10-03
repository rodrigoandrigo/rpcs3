#pragma once
// Local replacement for the retired desktop-wide precompiled header.
// No GSRender include here: shader translation must compile independently.
#include <d3dcompiler.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "D3D12Utils.h"
#include "D3D12Formats.h"

using pD3DCompile = decltype(&D3DCompile);
