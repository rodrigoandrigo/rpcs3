#include <windows.h>
#include <cstdio>

int main()
{
    constexpr SIZE_T blockSize = 0x0fff0000;
    HANDLE section = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE | SEC_RESERVE, blockSize, nullptr);
    if (!section) return 1;
    void* views[2]{};
    for (auto& view : views)
    {
        void* placeholder = VirtualAlloc2FromApp(nullptr, nullptr, blockSize,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        if (!placeholder) return 2;
        view = MapViewOfFile3FromApp(section, GetCurrentProcess(), placeholder,
            0, blockSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (view != placeholder) { std::printf("map error %lu\n", GetLastError()); return 3; }
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(view, &info, sizeof(info)) || info.State != MEM_RESERVE) return 4;
    }
    auto* guest = static_cast<unsigned char*>(views[0]);
    auto* sudo = static_cast<unsigned char*>(views[1]);
    if (!VirtualAllocFromApp(sudo, 0x10000, MEM_COMMIT, PAGE_READWRITE)) return 5;
    DWORD oldProtection{};
    if (!VirtualProtectFromApp(guest, 0x10000, PAGE_READWRITE, &oldProtection)) return 6;
    sudo[0x1000] = 0x42;
    if (guest[0x1000] != 0x42) return 7;
    guest[0x2000] = 0x27;
    if (sudo[0x2000] != 0x27) return 8;
    if (!VirtualProtectFromApp(guest, 0x1000, PAGE_NOACCESS, &oldProtection)) return 9;
    sudo[0] = 0x81; // Stack guard marker stays writable only through sudo.
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(guest, &info, sizeof(info)) || info.Protect != PAGE_NOACCESS) return 10;
    if (!VirtualQuery(guest + 0x10000, &info, sizeof(info)) || info.State != MEM_RESERVE) return 11;
    for (auto view : views)
    {
        if (!UnmapViewOfFile2(GetCurrentProcess(), view, MEM_PRESERVE_PLACEHOLDER)) return 12;
        if (!VirtualFree(view, 0, MEM_RELEASE)) return 13;
    }
    CloseHandle(section);
    std::puts("PASS: 256 MiB reserved dual views, partial commit, alias coherence, guard protection, teardown");
}
