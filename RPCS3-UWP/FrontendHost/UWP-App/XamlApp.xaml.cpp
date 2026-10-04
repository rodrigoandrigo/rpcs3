#include "XamlApp.xaml.h"
#include "XamlPage.xaml.h"

using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;

namespace UwpImGuiFrontend
{
XamlApp::XamlApp()
{
	// Xbox's automatic layout scaling enlarges the entire frontend. Disable it
	// before creating XAML content; normal display DPI handling is unchanged.
	if (Windows::System::Profile::AnalyticsInfo::VersionInfo->DeviceFamily == "Windows.Xbox" &&
		Windows::Foundation::Metadata::ApiInformation::IsMethodPresent(
			"Windows.UI.ViewManagement.ApplicationViewScaling", "TrySetDisableLayoutScaling"))
	{
		Windows::UI::ViewManagement::ApplicationViewScaling::TrySetDisableLayoutScaling(true);
	}
	InitializeComponent();
}

void XamlApp::OnLaunched(LaunchActivatedEventArgs^)
{
	auto window = Window::Current;
	Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()->SetDesiredBoundsMode(
		Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
	if (window->Content == nullptr)
	{
		window->Content = ref new XamlPage();
	}
	window->Activate();
}
}
