#include <Windows.h>
#include <cstdio>

int main()
{
    wchar_t directory[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"rpc", 0, path)) return 1;
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    DWORD bytes{};
    DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr);
    HANDLE section = CreateFileMappingFromApp(file, nullptr, PAGE_READWRITE, 0x800000000ull, nullptr);
    void* address = VirtualAllocFromApp(nullptr, 0x800000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!section || !address) return 2;
    VirtualFree(address, 0, MEM_RELEASE);
    void* shared = MapViewOfFile3FromApp(section, nullptr, address, 0,
        0x800000000ull, 0, PAGE_READWRITE, nullptr, 0);
    std::printf("Shared mapping: %p error %lu\n", shared, shared ? 0ul : GetLastError());
    if (shared) UnmapViewOfFile(shared);
    void* view = MapViewOfFile3FromApp(section, nullptr, address, 0,
        0x800000000ull, 0, PAGE_WRITECOPY, nullptr, 0);
    std::printf("Hook mapping: %p expected %p error %lu\n", view, address, view ? 0ul : GetLastError());
    if (view) UnmapViewOfFile(view);
    void* automatic = MapViewOfFileFromApp(section, FILE_MAP_COPY, 0, 0x800000000ull);
    std::printf("Automatic mapping: %p error %lu\n", automatic, automatic ? 0ul : GetLastError());
    if (automatic)
    {
        auto* data = static_cast<unsigned char*>(automatic);
        if (data[0] != 0 || data[0x7ffffffffull] != 0) return 4;
        data[0] = 0x5a;
        data[0x7ffffffffull] = 0xa5;
        UnmapViewOfFile(automatic);
        automatic = MapViewOfFileFromApp(section, FILE_MAP_COPY, 0, 0x800000000ull);
        if (!automatic) return 5;
        data = static_cast<unsigned char*>(automatic);
        if (data[0] != 0 || data[0x7ffffffffull] != 0) return 6;
        UnmapViewOfFile(automatic);
        std::puts("PASS UWP sparse hook mapping, boundary writes and COW reset");
    }
    CloseHandle(section);
    CloseHandle(file);
    return automatic ? 0 : 3;
}
