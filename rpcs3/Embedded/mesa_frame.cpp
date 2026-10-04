#include "stdafx.h"
#include "mesa_frame.h"
#include "Emu/RSX/GL/OpenGL.h"
#include "Emu/RSX/D3D12/D3D12Presentation.h"
#include <mutex>
#include <stdexcept>
#include <cstring>

// These imports resolve to the packaged Mesa shim, never Windows desktop WGL.
extern "C" __declspec(dllimport) void* WINAPI wglCreateContext(HDC);
extern "C" __declspec(dllimport) BOOL WINAPI wglDeleteContext(void*);
extern "C" __declspec(dllimport) BOOL WINAPI wglMakeCurrent(HDC, void*);
extern "C" __declspec(dllimport) PROC WINAPI wglGetProcAddress(LPCSTR);

namespace
{
template<typename T> T required(HMODULE module, const char* name)
{
    auto value = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!value) throw std::runtime_error(std::string("Mesa UWP export missing: ") + name);
    return value;
}
void check(HRESULT result, const char* operation)
{
    if (FAILED(result)) fmt::throw_exception("Mesa presentation %s failed (HRESULT 0x%x)", operation, static_cast<u32>(result));
}
class mesa_frame final : public GSFrameBase
{
    static constexpr u32 width = 1280, height = 720;
    using create_context = void* (WINAPI*)(HDC, void*, const int*);
    create_context m_create{};
    void* m_shared{};
    std::mutex m_context_mutex;
    bool m_shown = true;
    // Mesa UWP's HDC is an opaque key. No desktop HWND is created or used.
    HDC m_dc = reinterpret_cast<HDC>(1);
public:
    mesa_frame()
    {
        // Imported Mesa opengl32.dll loads Gallium before core initialization.
        auto module = GetModuleHandleW(L"gallium_wgl.dll");
        if (!module) throw std::runtime_error("Packaged Gallium D3D12 driver is missing");
        required<void (*)(int)>(module, "mesa_uwp_set_offscreen")(1);
        required<void (*)(void (*)(void*, const char*), void*)>(module, "mesa_uwp_set_log_callback")(
            [](void*, const char* message) { rsx_log.notice("Mesa: %s", message); }, nullptr);
        auto window = required<void (*)(void*, int, int)>(module, "uwp_set_window_reference");
        window(nullptr, width, height);
        auto pixel_format = required<int (WINAPI*)(HDC)>(module, "GetPixelFormat");
        if (!pixel_format(m_dc)) throw std::runtime_error("Mesa could not select a UWP pixel format");
        void* bootstrap = wglCreateContext(m_dc);
        if (!bootstrap) throw std::runtime_error("Mesa bootstrap context creation failed");
        if (!wglMakeCurrent(m_dc, bootstrap))
        {
            wglDeleteContext(bootstrap);
            throw std::runtime_error("Mesa bootstrap context binding failed");
        }
        m_create = reinterpret_cast<create_context>(wglGetProcAddress("wglCreateContextAttribsARB"));
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(bootstrap);
        if (!m_create) throw std::runtime_error("Mesa lacks WGL_ARB_create_context");
    }
    void close() override { delete this; }
    void reset() override {}
    bool shown() override { return m_shown; }
    void hide() override { m_shown = false; }
    void show() override { m_shown = true; }
    void toggle_fullscreen() override {}
    draw_context_t make_context() override
    {
        std::lock_guard lock(m_context_mutex);
        // OpenGL 4.3 core is the minimum needed by RPCS3; Mesa validates it.
        const int attributes[] = {0x2091, 4, 0x2092, 3, 0x9126, 1, 0};
        void* context = m_create(m_dc, m_shared, attributes);
        if (!context) throw std::runtime_error("Mesa Gallium D3D12 could not create an OpenGL 4.3 core context");
        if (!m_shared) m_shared = context;
        return context;
    }
    void set_current(draw_context_t context) override
    {
        if (!wglMakeCurrent(context ? m_dc : nullptr, context))
            throw std::runtime_error("Mesa context binding failed");
    }
    void delete_context(draw_context_t context) override
    {
        if (!context) return;
        std::lock_guard lock(m_context_mutex);
        wglMakeCurrent(nullptr, nullptr);
        if (!wglDeleteContext(context)) throw std::runtime_error("Mesa context deletion failed");
        if (m_shared == context) m_shared = nullptr;
    }
    void flip(draw_context_t, bool skip = false) override
    {
        if (skip) return;
        // Readback bridge: Gallium owns its GL device; the host owns the output
        // D3D12 device. Do not share resources across unmatched device identities.
        GLint framebuffer{}, read_buffer{}, pack_buffer{}, alignment{}, row_length{}, skip_rows{}, skip_pixels{};
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_READ_BUFFER, &read_buffer);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        std::vector<u8> pixels(width * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        glReadBuffer(read_buffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
        auto [device, queue] = d3d12::presentation().device_and_queue();
        using Microsoft::WRL::ComPtr;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> output, upload;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&output)), "output allocation");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 size{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = size; buffer.Height = 1; buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "upload allocation");
        u8* mapped{}; D3D12_RANGE empty{};
        check(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "upload map");
        for (u32 row = 0; row < height; ++row)
            std::memcpy(mapped + footprint.Offset + row * footprint.Footprint.RowPitch,
                pixels.data() + (height - 1 - row) * width * 4, width * 4);
        upload->Unmap(0, nullptr);
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "command list");
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        destination.pResource = output.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = output.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        check(list->Close(), "close commands");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        HANDLE event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (!event) throw std::runtime_error("Mesa upload event creation failed");
        ID3D12CommandList* commands[] = {list.Get()};
        queue->ExecuteCommandLists(1, commands);
        HRESULT signal = queue->Signal(fence.Get(), 1);
        HRESULT wait = SUCCEEDED(signal) ? fence->SetEventOnCompletion(1, event) : signal;
        if (SUCCEEDED(wait))
        {
            while (fence->GetCompletedValue() < 1)
            {
                const DWORD status = WaitForSingleObjectEx(event, 1000, FALSE);
                if (status == WAIT_FAILED) { wait = HRESULT_FROM_WIN32(GetLastError()); break; }
                wait = device->GetDeviceRemovedReason();
                if (FAILED(wait)) break;
            }
            if (SUCCEEDED(wait)) wait = device->GetDeviceRemovedReason();
        }
        CloseHandle(event);
        check(wait, "upload completion");
        d3d12::presentation().publish(output.Get());
    }
    int client_width() override { return width; }
    int client_height() override { return height; }
    f64 client_display_rate() override { return 60.; }
    bool has_alpha() override { return false; }
    display_handle_t handle() const override { return reinterpret_cast<HWND>(m_dc); }
    bool can_consume_frame() const override { return false; }
    void present_frame(std::vector<u8>&&, u32, u32, u32, bool) const override {}
    void take_screenshot(std::vector<u8>&&, u32, u32, bool) override {}
    void update_title(double = 0.) override {}
};
}
std::unique_ptr<GSFrameBase> make_mesa_frame() { return std::make_unique<mesa_frame>(); }
