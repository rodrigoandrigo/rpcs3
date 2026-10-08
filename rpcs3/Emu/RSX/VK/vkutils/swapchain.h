#pragma once

#if defined(RPCS3_UWP_DZN)
#include "swapchain_uwp.hpp"
#elif defined (_WIN32)
#include "swapchain_win32.hpp"
#elif defined (ANDROID)
#include "swapchain_android.hpp"
#elif defined (__APPLE__)
#include "swapchain_macos.hpp"
#else // Both linux and BSD families
#include "swapchain_unix.hpp"
#endif
