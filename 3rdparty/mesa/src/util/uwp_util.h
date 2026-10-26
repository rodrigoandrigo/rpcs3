#ifndef UWP_UTIL_H
#define UWP_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(_XBOX_UWP)
#define MESA_UWP_EXPORT __declspec(dllexport)
#else
#define MESA_UWP_EXPORT
#endif

MESA_UWP_EXPORT void* uwp_get_window_reference(void);
/* A failed COM marshaling request makes the window getter return NULL until
 * the host supplies a new reference. Width/height are published together. */
MESA_UWP_EXPORT void uwp_set_window_reference(void *window, int width, int height);
MESA_UWP_EXPORT void mesa_uwp_set_offscreen(int enabled);
MESA_UWP_EXPORT int mesa_uwp_is_offscreen(void);
typedef long (__cdecl *mesa_uwp_swapchain_attach_callback)(void *opaque,
                                                           void *swapchain);
MESA_UWP_EXPORT void mesa_uwp_set_swapchain_attach_callback(
   mesa_uwp_swapchain_attach_callback callback, void *opaque);
MESA_UWP_EXPORT long mesa_uwp_attach_swapchain(void *swapchain);
MESA_UWP_EXPORT void mesa_uwp_set_log_callback(
   void (*callback)(void *opaque, const char *message), void *opaque);
/* Unknown dimensions return -1 on WinRT failure or after a one-second UI
 * dispatch timeout. The host can supply dimensions to avoid dispatching. */
MESA_UWP_EXPORT int uwp_get_height(void);
MESA_UWP_EXPORT int uwp_get_width(void);

#ifdef __cplusplus
}
#endif

#endif
