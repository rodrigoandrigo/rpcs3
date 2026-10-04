/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "d3d12_wgl_public.h"

#include <new>

#include <windows.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#include <directx/d3d12.h>
#include <wrl.h>
#include <dxguids/dxguids.h>
#include <windows.ui.xaml.media.dxinterop.h>

#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "frontend/api.h"
#include "frontend/winsys_handle.h"

#include "stw_device.h"
#include "stw_pixelformat.h"
#include "stw_winsys.h"

#include "d3d12/d3d12_format.h"
#include "d3d12/d3d12_resource.h"
#include "d3d12/d3d12_screen.h"
#include "util/uwp_util.h"

using Microsoft::WRL::ComPtr;
constexpr uint32_t num_buffers = 2;

struct d3d12_wgl_framebuffer {
   struct stw_winsys_framebuffer base;

   struct d3d12_screen *screen;
   enum pipe_format pformat;
   HWND window;
   ComPtr<IDXGISwapChain3> swapchain;
   pipe_resource *buffers[num_buffers];
};

static struct d3d12_wgl_framebuffer *
d3d12_wgl_framebuffer(struct stw_winsys_framebuffer *fb)
{
   return (struct d3d12_wgl_framebuffer *)fb;
}

static void
d3d12_wgl_framebuffer_release_buffers(struct d3d12_wgl_framebuffer *framebuffer)
{
   for (int i = 0; i < num_buffers; ++i) {
      if (framebuffer->buffers[i]) {
         d3d12_resource_release(d3d12_resource(framebuffer->buffers[i]));
         pipe_resource_reference(&framebuffer->buffers[i], NULL);
      }
   }
}

static bool
d3d12_wgl_framebuffer_wrap_buffers(struct d3d12_wgl_framebuffer *framebuffer)
{
   auto pscreen = &framebuffer->screen->base;

   for (int i = 0; i < num_buffers; ++i) {
      ID3D12Resource *res = NULL;
      if (FAILED(framebuffer->swapchain->GetBuffer(i, IID_PPV_ARGS(&res))) || !res)
         return false;

      struct winsys_handle handle;
      memset(&handle, 0, sizeof(handle));
      handle.type = WINSYS_HANDLE_TYPE_D3D12_RES;
      handle.format = framebuffer->pformat;
      handle.com_obj = res;

      D3D12_RESOURCE_DESC res_desc = GetDesc(res);

      struct pipe_resource templ;
      memset(&templ, 0, sizeof(templ));
      templ.target = PIPE_TEXTURE_2D;
      templ.format = framebuffer->pformat;
      templ.width0 = res_desc.Width;
      templ.height0 = res_desc.Height;
      templ.depth0 = 1;
      templ.array_size = res_desc.DepthOrArraySize;
      templ.nr_samples = res_desc.SampleDesc.Count;
      templ.last_level = res_desc.MipLevels - 1;
      templ.bind = PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_RENDER_TARGET;
      templ.usage = PIPE_USAGE_DEFAULT;
      templ.flags = 0;

      /* resource_from_handle returns the initial pipe reference and consumes
       * the COM reference supplied in handle.com_obj.  Taking another pipe
       * reference here leaked one complete swapchain resource on every
       * resize. */
      pipe_resource *buffer = pscreen->resource_from_handle(
         pscreen, &templ, &handle, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
      if (!buffer)
         return false;
      framebuffer->buffers[i] = buffer;
   }

   return true;
}

static void
d3d12_wgl_framebuffer_destroy(struct stw_winsys_framebuffer *fb,
                              pipe_context *ctx)
{
   struct d3d12_wgl_framebuffer *framebuffer = d3d12_wgl_framebuffer(fb);
   struct pipe_fence_handle *fence = NULL;

   if (ctx) {
      /* Ensure all resources are flushed */
      ctx->flush(ctx, &fence, PIPE_FLUSH_HINT_FINISH);
      if (fence) {
         ctx->screen->fence_finish(ctx->screen, ctx, fence, OS_TIMEOUT_INFINITE);
         ctx->screen->fence_reference(ctx->screen, &fence, NULL);
      }
      d3d12_screen_reclaim_completed(framebuffer->screen);
   }

   d3d12_wgl_framebuffer_release_buffers(framebuffer);

   delete framebuffer;
}

static void
d3d12_wgl_framebuffer_resize(stw_winsys_framebuffer *fb,
                             pipe_context *ctx,
                             pipe_resource *templ)
{
   struct d3d12_wgl_framebuffer *framebuffer = d3d12_wgl_framebuffer(fb);
   struct d3d12_dxgi_screen *screen = d3d12_dxgi_screen(framebuffer->screen);

   DXGI_SWAP_CHAIN_DESC1 desc = {};
   desc.BufferCount = num_buffers;
   desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
   /* Composition swapchains do not need desktop tearing support.  Keeping
    * this flag clear also avoids DXGI_ERROR_INVALID_CALL on Xbox/UWP
    * runtimes that do not expose tearing for SwapChainPanel composition. */
   desc.Flags = 0;
   desc.Format = d3d12_get_format(templ->format);
   desc.Width = templ->width0;
   desc.Height = templ->height0;
   desc.SampleDesc.Count = 1;
   desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

   framebuffer->pformat = templ->format;

   if (!framebuffer->swapchain) {
      ComPtr<IDXGISwapChain1> swapchain1;
      IUnknown *window = reinterpret_cast<IUnknown *>(framebuffer->window);
      HRESULT result = window ? screen->factory->CreateSwapChainForComposition(
         screen->base.cmdqueue, &desc, nullptr, &swapchain1) : E_POINTER;
      if (SUCCEEDED(result))
         result = (HRESULT)mesa_uwp_attach_swapchain(swapchain1.Get());
      if (FAILED(result)) {
         debug_printf("D3D12: failed to create/attach UWP swapchain (hr=0x%08lx)\n",
                      (unsigned long)result);
         return;
      }

      result = swapchain1.As(&framebuffer->swapchain);
      if (FAILED(result) || !framebuffer->swapchain) {
         debug_printf("D3D12: failed to query IDXGISwapChain3 (hr=0x%08lx)\n",
                      (unsigned long)result);
         return;
      }
      debug_printf("D3D12: UWP swapchain attached to XAML panel (%ux%u)\n",
                   desc.Width, desc.Height);

#ifdef UWP_HDR
      ComPtr<IDXGISwapChain4> swap_chain4;
      if (SUCCEEDED(swapchain1.As(&swap_chain4)))
         swap_chain4->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
#endif
   } else {
      struct pipe_fence_handle *fence = NULL;

      /* Ensure all resources are flushed */
      ctx->flush(ctx, &fence, PIPE_FLUSH_HINT_FINISH);
      if (fence) {
         ctx->screen->fence_finish(ctx->screen, ctx, fence, OS_TIMEOUT_INFINITE);
         ctx->screen->fence_reference(ctx->screen, &fence, NULL);
      }

      /* fence_finish only waits for the GPU.  Reclaim the completed batches
       * as well so they release their references to the old backbuffers
       * before ResizeBuffers. */
      d3d12_screen_reclaim_completed(framebuffer->screen);

      d3d12_wgl_framebuffer_release_buffers(framebuffer);

      if (FAILED(framebuffer->swapchain->ResizeBuffers(num_buffers, desc.Width, desc.Height, desc.Format, desc.Flags))) {
         debug_printf("D3D12: failed to resize swapchain");
         return;
      }
   }

   if (!d3d12_wgl_framebuffer_wrap_buffers(framebuffer)) {
      debug_printf("D3D12: failed to wrap swapchain buffers");
      d3d12_wgl_framebuffer_release_buffers(framebuffer);
   }
}

static bool
d3d12_wgl_framebuffer_present(stw_winsys_framebuffer *fb, int interval)
{
   static bool first_present = true;
   auto framebuffer = d3d12_wgl_framebuffer(fb);
   if (!framebuffer->swapchain) {
      debug_printf("D3D12: Cannot present; no swapchain");
      return false;
   }

   HRESULT result = framebuffer->swapchain->Present(interval < 1 ? 0 : interval, 0);
   if (FAILED(result)) {
      debug_printf("D3D12: UWP swapchain Present failed (hr=0x%08lx)\n",
                   (unsigned long)result);
      return false;
   }
   if (first_present) {
      debug_printf("D3D12: first UWP swapchain frame presented\n");
      first_present = false;
   }
   return true;
}

static struct pipe_resource *
d3d12_wgl_framebuffer_get_resource(struct stw_winsys_framebuffer *pframebuffer,
                                   st_attachment_type statt)
{
   auto framebuffer = d3d12_wgl_framebuffer(pframebuffer);

   if (!framebuffer->swapchain)
      return nullptr;

   UINT index = framebuffer->swapchain->GetCurrentBackBufferIndex();
   if (statt == ST_ATTACHMENT_FRONT_LEFT)
      index = !index;

   if (!framebuffer->buffers[index])
      return nullptr;

   pipe_reference(NULL, &framebuffer->buffers[index]->reference);
   return framebuffer->buffers[index];
}

struct stw_winsys_framebuffer *
d3d12_wgl_create_framebuffer(struct pipe_screen *screen,
                             HWND hWnd,
                             int iPixelFormat)
{
   const struct stw_pixelformat_info *pfi =
      stw_pixelformat_get_info(iPixelFormat);
   if (!(pfi->pfd.dwFlags & PFD_DOUBLEBUFFER) ||
       (pfi->pfd.dwFlags & PFD_SUPPORT_GDI)) {
      debug_printf("D3D12: rejected UWP pixel format %d (flags=0x%08lx)\n",
                   iPixelFormat, (unsigned long)pfi->pfd.dwFlags);
      return NULL;
   }

   if (pfi->stvis.color_format != PIPE_FORMAT_B8G8R8A8_UNORM &&
       pfi->stvis.color_format != PIPE_FORMAT_R8G8B8A8_UNORM &&
       pfi->stvis.color_format != PIPE_FORMAT_R10G10B10A2_UNORM &&
       pfi->stvis.color_format != PIPE_FORMAT_R16G16B16A16_FLOAT) {
      debug_printf("D3D12: rejected UWP pixel format %d (format=%d)\n",
                   iPixelFormat, pfi->stvis.color_format);
      return NULL;
   }

   struct d3d12_wgl_framebuffer *fb =
      new (std::nothrow) struct d3d12_wgl_framebuffer();
   if (!fb)
      return NULL;

   fb->window = hWnd;
   fb->screen = d3d12_screen(screen);
   fb->base.destroy = d3d12_wgl_framebuffer_destroy;
   fb->base.resize = d3d12_wgl_framebuffer_resize;
   fb->base.present = d3d12_wgl_framebuffer_present;
   fb->base.get_resource = d3d12_wgl_framebuffer_get_resource;

   debug_printf("D3D12: created UWP WGL framebuffer for pixel format %d\n",
                iPixelFormat);

   return &fb->base;
}
