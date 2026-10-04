#include <Windows.h>
#include <GL/gl.h>
#include <filesystem>
#include <iostream>
#include <string>

// Native regression only, not an AppContainer or Xbox test.
int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3) return 2;
    if (argc == 3) AddDllDirectory(std::filesystem::absolute(argv[2]).c_str());
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // The UWP host imports these system components before loading Mesa.
    LoadLibraryW(L"dxgi.dll");
    LoadLibraryW(L"d3d12.dll");
    const auto root = std::filesystem::absolute(argv[1]);
    const auto flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    if (!LoadLibraryExW((root / L"dxil.dll").c_str(), nullptr, flags)) return 3;
    auto driver = LoadLibraryExW((root / L"gallium_wgl.dll").c_str(), nullptr, flags);
    auto module = LoadLibraryExW((root / L"opengl32.dll").c_str(), nullptr, flags);
    if (!driver || !module) { std::cerr << "Load failed: " << GetLastError(); return 3; }
    auto offscreen = reinterpret_cast<void (*)(int)>(GetProcAddress(driver, "mesa_uwp_set_offscreen"));
    if (!offscreen) return 4;
    offscreen(1);
    auto window = reinterpret_cast<void (*)(void*, int, int)>(GetProcAddress(driver, "uwp_set_window_reference"));
    auto format = reinterpret_cast<int (WINAPI*)(HDC)>(GetProcAddress(driver, "GetPixelFormat"));
    auto create = reinterpret_cast<HGLRC (WINAPI*)(HDC)>(GetProcAddress(module, "wglCreateContext"));
    auto bind = reinterpret_cast<BOOL (WINAPI*)(HDC, HGLRC)>(GetProcAddress(module, "wglMakeCurrent"));
    auto destroy = reinterpret_cast<BOOL (WINAPI*)(HGLRC)>(GetProcAddress(module, "wglDeleteContext"));
    auto proc = reinterpret_cast<PROC (WINAPI*)(LPCSTR)>(GetProcAddress(module, "wglGetProcAddress"));
    if (!window || !format || !create || !bind || !destroy || !proc) return 4;
    const auto dc = reinterpret_cast<HDC>(1);
    std::cout << "Loaded packaged Mesa DLLs\n" << std::flush;
    auto log = reinterpret_cast<void (*)(void (*)(void*, const char*), void*)>(GetProcAddress(driver, "mesa_uwp_set_log_callback"));
    if (log) log([](void*, const char* message) { std::cerr << message << std::flush; }, nullptr);
    window(nullptr, 64, 64);
    const int selected = format(dc);
    std::cout << "Pixel format: " << selected << '\n' << std::flush;
    if (!selected) return 5;
    auto bootstrap = create(dc);
    if (!bootstrap || !bind(dc, bootstrap)) return 6;
    auto attributes_create = reinterpret_cast<HGLRC (WINAPI*)(HDC, HGLRC, const int*)>(proc("wglCreateContextAttribsARB"));
    bind(nullptr, nullptr); destroy(bootstrap);
    if (!attributes_create) return 7;
    const int attributes[] = {0x2091, 4, 0x2092, 3, 0x9126, 1, 0};
    auto context = attributes_create(dc, nullptr, attributes);
    if (!context || !bind(dc, context)) return 8;
    auto get_string = reinterpret_cast<decltype(&glGetString)>(GetProcAddress(module, "glGetString"));
    auto clear_color = reinterpret_cast<decltype(&glClearColor)>(GetProcAddress(module, "glClearColor"));
    auto clear = reinterpret_cast<decltype(&glClear)>(GetProcAddress(module, "glClear"));
    auto read = reinterpret_cast<decltype(&glReadPixels)>(GetProcAddress(module, "glReadPixels"));
    if (!get_string || !clear_color || !clear || !read) return 4;
    const std::string renderer(reinterpret_cast<const char*>(get_string(GL_RENDERER)));
    std::cout << "Renderer: " << renderer << "\nVersion: " << get_string(GL_VERSION) << '\n';
    clear_color(.25f, .5f, .75f, 1.f); clear(GL_COLOR_BUFFER_BIT);
    unsigned char pixel[4]{};
    read(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    bool success = renderer.find("D3D12") != std::string::npos &&
        pixel[0] >= 63 && pixel[0] <= 65 && pixel[1] >= 127 && pixel[1] <= 129 &&
        pixel[2] >= 190 && pixel[2] <= 192 && pixel[3] == 255;
    bind(nullptr, nullptr); destroy(context);
    std::cout << (success ? "PASS" : "FAIL") << " OpenGL 4.3 Gallium D3D12 clear/readback\n";
    return success ? 0 : 9;
}
