#include "XamlApp.xaml.h"
#include "XamlPage.xaml.h"

using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;

namespace UwpImGuiFrontend
{
XamlApp::XamlApp()
{
	InitializeComponent();
}

void XamlApp::OnLaunched(LaunchActivatedEventArgs^)
{
	auto window = Window::Current;
	if (window->Content == nullptr)
	{
		window->Content = ref new XamlPage();
	}
	window->Activate();
}
}
