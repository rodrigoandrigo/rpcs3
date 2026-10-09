#pragma once

#include "XamlPage.g.h"
#include <cstdint>
#include <unknwn.h>
#include <windows.h>

namespace UwpImGuiFrontend
{
public ref class XamlPage sealed
{
public:
	XamlPage();
	virtual ~XamlPage();

private:
	void OnLoaded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
	void UpdatePanelSize();
	using ResizeRuntime = void (*)(void*, uint32_t, uint32_t, float, float);
	using CreateRuntime = void* (*)(::IUnknown*, uint32_t, uint32_t, float);
	using RunRuntime = void (*)(void*);
	using StopRuntime = void (*)(void*);
	using DestroyRuntime = void (*)(void*);
	HMODULE m_runtimeModule = nullptr;
	RunRuntime m_run = nullptr;
	StopRuntime m_stop = nullptr;
	DestroyRuntime m_destroy = nullptr;
	ResizeRuntime m_resize = nullptr;
	void* m_runtime = nullptr;
	bool m_finished = false;
	Windows::Foundation::IAsyncAction^ m_worker;
};
}
