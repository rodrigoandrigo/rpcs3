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
	Platform::WeakReference weak(this);
	Loaded += ref new Windows::UI::Xaml::RoutedEventHandler(
		[weak](Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args)
		{ if (auto page = weak.Resolve<XamlPage>()) page->OnLoaded(sender, args); });
	FrontendPanel->SizeChanged += ref new Windows::UI::Xaml::SizeChangedEventHandler(
		[weak](Platform::Object^, Windows::UI::Xaml::SizeChangedEventArgs^)
		{ if (auto page = weak.Resolve<XamlPage>()) page->UpdatePanelSize(); });
	FrontendPanel->CompositionScaleChanged += ref new Windows::Foundation::TypedEventHandler<
		Windows::UI::Xaml::Controls::SwapChainPanel^, Platform::Object^>(
		[weak](Windows::UI::Xaml::Controls::SwapChainPanel^, Platform::Object^)
		{ if (auto page = weak.Resolve<XamlPage>()) page->UpdatePanelSize(); });
}

void XamlPage::OnLoaded(Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^)
{
	if (m_runtime || m_finished)
		return;
	try
	{
		StatusText->Text = "Loading RPCS3 runtime...";
		if (!m_runtimeModule) m_runtimeModule = LoadPackagedLibrary(L"RPCS3FrontendRuntime.dll", 0);
		if (!m_runtimeModule)
		{
			StatusText->Text = "RPCS3 runtime could not be loaded.";
			return;
		}
		auto create = reinterpret_cast<CreateRuntime>(GetProcAddress(m_runtimeModule, "rpcs3_frontend_create"));
		m_run = reinterpret_cast<RunRuntime>(GetProcAddress(m_runtimeModule, "rpcs3_frontend_run"));
		m_stop = reinterpret_cast<StopRuntime>(GetProcAddress(m_runtimeModule, "rpcs3_frontend_stop"));
		m_destroy = reinterpret_cast<DestroyRuntime>(GetProcAddress(m_runtimeModule, "rpcs3_frontend_destroy"));
		m_resize = reinterpret_cast<ResizeRuntime>(GetProcAddress(m_runtimeModule, "rpcs3_frontend_resize"));
		if (!create || !m_run || !m_stop || !m_destroy || !m_resize)
		{
			StatusText->Text = "RPCS3 runtime API is incomplete.";
			return;
		}
		StatusText->Text = "Creating D3D12 frontend...";
		auto inspectable = reinterpret_cast<IInspectable*>(FrontendPanel);
		const float scale = FrontendPanel->CompositionScaleX;
		const auto width = static_cast<uint32_t>(std::max(1.0,
			std::round(FrontendPanel->ActualWidth * scale)));
		const auto height = static_cast<uint32_t>(std::max(1.0,
			std::round(FrontendPanel->ActualHeight * FrontendPanel->CompositionScaleY)));
		m_runtime = create(inspectable, width, height, scale);
		if (!m_runtime)
		{
			StatusText->Text = "RPCS3 frontend initialization failed.";
			return;
		}
		UpdatePanelSize();
		StatusText->Text = "Starting RPCS3 frontend...";
		m_worker = ThreadPool::RunAsync(ref new WorkItemHandler(
			[this](Windows::Foundation::IAsyncAction^)
			{
				m_run(m_runtime);
			}));
		m_worker->Completed = ref new Windows::Foundation::AsyncActionCompletedHandler(
			[this](Windows::Foundation::IAsyncAction^, Windows::Foundation::AsyncStatus)
			{
				Dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
					ref new Windows::UI::Core::DispatchedHandler([this]()
					{
						m_finished = true;
						auto runtime = m_runtime;
						m_runtime = nullptr;
						m_worker = nullptr;
						// Run has joined/released the core before returning. Destroy
						// the host only now, on the XAML thread, never while rendering.
						if (runtime && m_destroy) m_destroy(runtime);
					}));
			});
		StatusOverlay->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
		Focus(Windows::UI::Xaml::FocusState::Programmatic);
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

void XamlPage::UpdatePanelSize()
{
	if (!m_runtime || !m_resize || FrontendPanel->ActualWidth <= 0 || FrontendPanel->ActualHeight <= 0)
		return;
	m_resize(m_runtime,
		static_cast<uint32_t>(std::max(1.0, std::round(FrontendPanel->ActualWidth * FrontendPanel->CompositionScaleX))),
		static_cast<uint32_t>(std::max(1.0, std::round(FrontendPanel->ActualHeight * FrontendPanel->CompositionScaleY))),
		FrontendPanel->CompositionScaleX, FrontendPanel->CompositionScaleY);
}

XamlPage::~XamlPage()
{
	if (m_runtime && m_stop)
		m_stop(m_runtime);
	// The runtime module remains loaded until AppContainer teardown so the
	// worker can never execute code from an unloaded image.
}
}
