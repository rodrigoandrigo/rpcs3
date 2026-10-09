// Native lifecycle regression, not installed UWP/Xbox gameplay validation.
#include <Windows.h>
#include <psapi.h>
#include "../core_api.h"
#include <filesystem>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) return 2;
    if (argc == 3) AddDllDirectory(std::filesystem::absolute(argv[2]).c_str());
    const auto path = std::filesystem::absolute(argv[1]);
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) { std::printf("LoadLibrary error=%lu\n", GetLastError()); return 3; }
    auto register_callbacks = reinterpret_cast<decltype(&rpcs3_core_set_callbacks)>(GetProcAddress(module, "rpcs3_core_set_callbacks"));
    auto release = reinterpret_cast<decltype(&rpcs3_core_release)>(GetProcAddress(module, "rpcs3_core_release"));
    if (!register_callbacks || !release) return 4;
    DWORD baseline = 0;
    SIZE_T initialPrivate = 0, finalPrivate = 0;
    for (unsigned cycle = 0; cycle < 33; ++cycle)
    {
        rpcs3_core_callbacks callbacks{};
        callbacks.struct_size = sizeof(callbacks);
        callbacks.api_version = 2;
        if (register_callbacks(&callbacks) != RPCS3_CORE_OK) return 5;
        // Simulate failure after callback registration but before initialize.
        const auto deadline = GetTickCount64() + 5000;
        int result;
        do
        {
            result = release();
            if (result == RPCS3_CORE_BUSY) Sleep(1);
        } while (result == RPCS3_CORE_BUSY && GetTickCount64() < deadline);
        if (result != RPCS3_CORE_OK) { std::printf("release=%d cycle=%u\n", result, cycle); return 6; }
        DWORD handles = 0;
        if (!GetProcessHandleCount(GetCurrentProcess(), &handles)) return 7;
        if (!cycle) baseline = handles;
        if (handles > baseline) { std::printf("Handle leak: %lu -> %lu\n", baseline, handles); return 8; }
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return 9;
        if (!cycle) initialPrivate = counters.PrivateUsage;
        finalPrivate = counters.PrivateUsage;
    }
    std::printf("Private bytes after warm-up: %zu -> %zu\n", initialPrivate, finalPrivate);
    FreeLibrary(module);
    std::puts("PASS: 33 callback-register/release cycles, no handle growth");
    return 0;
}
