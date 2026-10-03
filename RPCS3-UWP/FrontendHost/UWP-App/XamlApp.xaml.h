#pragma once

#include "XamlApp.g.h"

namespace UwpImGuiFrontend
{
ref class XamlApp sealed
{
public:
	XamlApp();

protected:
	virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ args) override;
};
}
