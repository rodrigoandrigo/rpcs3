#include "XamlPage.xaml.h"
#include <algorithm>
#include <cmath>
#include <windows.ui.xaml.controls.h>
#include <windows.graphics.display.h>
using namespace Windows::System::Threading;

namespace UwpImGuiFrontend
{
XamlPage::XamlPage()
{
	InitializeComponent();
	Loaded += ref new Windows::UI::Xaml::RoutedEventHandler(this, &XamlPage::OnLoaded);
}

void XamlPage::OnLoaded(Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^)
{
	if (m_runtime)
		return;
	try
	{
		StatusText->Text = "Loading RPCS3 runtime...";
		m_runtimeModule = LoadPackagedLibrary(L"RPCS3FrontendRuntime.dll", 0);
		if (!m_runtimeModule)
		{
			StatusText->Text = "RPCS3 runtime could not be loaded.";
			return;
		}
		auto create = reinterpret_cast<CreateRuntime>(GetProcAddress(m_runtimeModule, "rpcs3_frontend_create"));
		m_run = reinterpret_cast<RunRuntime>(GetProcAddress(m_runtimeModule, "rpcs3_frontend_run"));
		m_stop = reinterpret_cast<StopRuntime>(GetProcAddress(m_runtimeModule, "rpcs3_frontend_stop"));
		m_destroy = reinterpret_cast<DestroyRuntime>(GetProcAddress(m_runtimeModule, "rpcs3_frontend_destroy"));
		if (!create || !m_run || !m_stop || !m_destroy)
		{
			StatusText->Text = "RPCS3 runtime API is incomplete.";
			return;
		}
		StatusText->Text = "Creating D3D12 frontend...";
		auto inspectable = reinterpret_cast<IInspectable*>(FrontendPanel);
		auto display = Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
		float scale = static_cast<float>(display->RawPixelsPerViewPixel);
		if (!(scale > 0.0f))
			scale = static_cast<float>(display->LogicalDpi / 96.0f);
		const auto width = static_cast<uint32_t>(std::max(1.0,
			std::round(FrontendPanel->ActualWidth * scale)));
		const auto height = static_cast<uint32_t>(std::max(1.0,
			std::round(FrontendPanel->ActualHeight * scale)));
		m_runtime = create(inspectable, width, height, scale);
		if (!m_runtime)
		{
			StatusText->Text = "RPCS3 frontend initialization failed.";
			return;
		}
		StatusText->Text = "Starting RPCS3 frontend...";
		m_worker = ThreadPool::RunAsync(ref new WorkItemHandler(
			[this](Windows::Foundation::IAsyncAction^)
			{
				m_run(m_runtime);
			}));
		StatusOverlay->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
	}
	catch (Platform::Exception^ error)
	{
		StatusText->Text = "RPCS3 startup failed: " + error->Message;
	}
	catch (...)
	{
		StatusText->Text = "RPCS3 native startup failed. The frontend remains available.";
	}
}

XamlPage::~XamlPage()
{
	if (m_runtime && m_stop)
		m_stop(m_runtime);
	// The runtime module remains loaded until AppContainer teardown so the
	// worker can never execute code from an unloaded image.
}
}
