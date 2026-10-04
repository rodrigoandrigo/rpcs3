#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS

#include "uwp_util.h"

#include <atomic>
#include <mutex>
#include <unknwn.h>
#include <roapi.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Graphics.Display.Core.h>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::System::Profile;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::Graphics::Display::Core;

static CoreWindow _uwpCoreWind = nullptr;
static Microsoft::WRL::ComPtr<IAgileReference> _uwpWindowReference;
static std::mutex _uwpWindowReferenceMutex;
static mesa_uwp_swapchain_attach_callback _uwpSwapchainAttach = nullptr;
static void *_uwpSwapchainAttachOpaque = nullptr;
static DisplayInformation _uwpDisplayInfo = nullptr;
static HdmiDisplayInformation _uwpHdi = nullptr;
static HdmiDisplayMode _uwpHdm = nullptr;
static int current_height = -1;
static int current_width = -1;
static std::atomic<bool> offscreen_drawable{false};

extern "C" void mesa_uwp_set_offscreen(int enabled)
{
   offscreen_drawable.store(enabled != 0);
}

extern "C" int mesa_uwp_is_offscreen(void)
{
   return offscreen_drawable.load();
}

static CoreWindow uwp_get_corewindow()
{
   if (!_uwpCoreWind) {
      _uwpCoreWind = CoreWindow::GetForCurrentThread();
   }

   return _uwpCoreWind;
}

extern "C" void* uwp_get_window_reference(void)
{
   /* The XAML panel is supplied by the UI apartment, while Mesa creates its
    * framebuffer on the emulator thread. Resolve an apartment-correct COM
    * proxy on every calling thread instead of sharing the raw XAML pointer. */
   thread_local Microsoft::WRL::ComPtr<IUnknown> resolved_window;
   Microsoft::WRL::ComPtr<IAgileReference> agile_reference;
   {
      std::lock_guard<std::mutex> lock(_uwpWindowReferenceMutex);
      agile_reference = _uwpWindowReference;
   }
   if (agile_reference) {
      resolved_window.Reset();
      if (SUCCEEDED(agile_reference->Resolve(IID_PPV_ARGS(&resolved_window))))
         return resolved_window.Get();
      return nullptr;
   }

   CoreWindow window = uwp_get_corewindow();
   if (!window)
      return nullptr;

   return winrt::get_abi(window);
}

extern "C" void uwp_set_window_reference(void *window, int width, int height)
{
   IUnknown *reference = static_cast<IUnknown *>(window);
   Microsoft::WRL::ComPtr<IAgileReference> agile_reference;
   if (reference) {
      RoGetAgileReference(AGILEREFERENCE_DEFAULT, IID_IUnknown, reference,
                          &agile_reference);
   }
   {
      std::lock_guard<std::mutex> lock(_uwpWindowReferenceMutex);
      _uwpWindowReference = agile_reference;
   }
   current_width = width;
   current_height = height;
}

extern "C" void mesa_uwp_set_swapchain_attach_callback(
   mesa_uwp_swapchain_attach_callback callback, void *opaque)
{
   std::lock_guard<std::mutex> lock(_uwpWindowReferenceMutex);
   _uwpSwapchainAttach = callback;
   _uwpSwapchainAttachOpaque = opaque;
}

extern "C" long mesa_uwp_attach_swapchain(void *swapchain)
{
   mesa_uwp_swapchain_attach_callback callback;
   void *opaque;
   {
      std::lock_guard<std::mutex> lock(_uwpWindowReferenceMutex);
      callback = _uwpSwapchainAttach;
      opaque = _uwpSwapchainAttachOpaque;
   }
   return callback ? callback(opaque, swapchain) : E_NOTIMPL;
}

static HdmiDisplayInformation uwp_get_hdi()
{
   if (!_uwpHdi) {
      _uwpHdi = HdmiDisplayInformation::GetForCurrentView();
   }

   return _uwpHdi;
}

static HdmiDisplayMode uwp_get_hdm()
{
   if (!_uwpHdm) {
      HdmiDisplayInformation hdi = uwp_get_hdi();
      if (hdi) {
         _uwpHdm = hdi.GetCurrentDisplayMode();
      }
   }

   return _uwpHdm;
}

static DisplayInformation uwp_get_displayInfo()
{
   if (!_uwpDisplayInfo) {
      _uwpDisplayInfo = DisplayInformation::GetForCurrentView();
   }

   return _uwpDisplayInfo;
}

static bool is_running_on_xbox(void)
{
   hstring device_family = AnalyticsInfo::VersionInfo().DeviceFamily();
   return (device_family == L"Windows.Xbox");
}

extern "C" int uwp_get_height(void)
{
   if (current_height != -1)
      return current_height;

   /* This function must be performed within UI thread,
    * otherwise it will cause a crash in specific cases
    * https://github.com/libretro/RetroArch/issues/13491 */
   float surface_scale = 0;
   int ret = -1;
   std::atomic<bool> finished{false};
   CoreDispatcher dispatcher =
         CoreApplication::MainView().CoreWindow().Dispatcher();
   dispatcher.RunAsync(
         CoreDispatcherPriority::Normal,
         DispatchedHandler([&surface_scale, &ret, &finished]()
            {
               if (is_running_on_xbox())
               {
                  HdmiDisplayMode hdm = uwp_get_hdm();
                  if (hdm)
                     ret = static_cast<int>(hdm.ResolutionHeightInRawPixels());
               }

               if (ret == -1)
               {
                  const LONG32 resolution_scale = static_cast<LONG32>(uwp_get_displayInfo().ResolutionScale());
                  surface_scale                 = static_cast<float>(resolution_scale) / 100.0f;
                  ret                           = static_cast<LONG32>(
                        uwp_get_corewindow().Bounds().Height
                        * surface_scale);
               }
               finished.store(true);
            }));
   while (!finished.load())
   {
      dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
   }
   current_height = ret;
   return ret;
}

extern "C" int uwp_get_width(void)
{
   if (current_width != -1)
      return current_width;

   /* This function must be performed within UI thread,
    * otherwise it will cause a crash in specific cases
    * https://github.com/libretro/RetroArch/issues/13491 */
   float surface_scale = 0;
   int returnValue = -1;
   std::atomic<bool> finished{false};
   CoreDispatcher dispatcher =
         CoreApplication::MainView().CoreWindow().Dispatcher();
   dispatcher.RunAsync(
         CoreDispatcherPriority::Normal,
         DispatchedHandler([&surface_scale, &returnValue, &finished]()
            {
               if (is_running_on_xbox())
               {
                  HdmiDisplayMode hdm = uwp_get_hdm();
                  if (hdm)
                     returnValue = static_cast<int>(hdm.ResolutionWidthInRawPixels());
               }

               if (returnValue == -1)
               {
                  const LONG32 resolution_scale = static_cast<LONG32>(uwp_get_displayInfo().ResolutionScale());
                  surface_scale = static_cast<float>(resolution_scale) / 100.0f;
                  returnValue   = static_cast<LONG32>(
                        uwp_get_corewindow().Bounds().Width
                        * surface_scale);
               }
               finished.store(true);
            }));
   while (!finished.load())
   {
      dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
   }

   current_width = returnValue;
   return returnValue;
}
