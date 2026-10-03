#pragma once

#include <d3d12.h>
#include <cassert>
#include <wrl/client.h>
#include "Emu/Memory/vm.h"
#include "Emu/RSX/GCM.h"
#include <locale>
#include <winrt/base.h>
#include "D3D12BufferCopy.h"


using namespace Microsoft::WRL;
namespace d3d12
{
	// Only for synchronous SDK calls: a temporary bound to this reference
	// lives through the complete call expression. Never retain this pointer.
	template <typename T>
	const T* address_of(const T& value) noexcept { return &value; }
}
extern ID3D12Device* g_d3d12_device;

inline std::string get_hresult_message(HRESULT hr)
{
	if (hr == DXGI_ERROR_DEVICE_REMOVED && g_d3d12_device)
	{
		hr = g_d3d12_device->GetDeviceRemovedReason();
		return fmt::format("D3D12 device was removed with error status 0x%X", static_cast<u32>(hr));
	}

	return winrt::to_string(winrt::hresult_error(winrt::hresult{static_cast<int32_t>(hr)}).message());
}

inline void check_hresult(HRESULT result)
{
    if (FAILED(result)) fmt::throw_exception("HRESULT 0x%08X: %s", static_cast<u32>(result), get_hresult_message(result));
}
#define CHECK_HRESULT(expr) check_hresult((expr))

