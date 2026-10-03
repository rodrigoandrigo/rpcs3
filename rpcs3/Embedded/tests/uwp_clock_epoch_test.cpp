#include <Windows.h>
#include <intrin.h>
#include <cstdio>

int main()
{
    LARGE_INTEGER before{}, epoch{}, frequency{}, after{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&before)) return 1;
    do { if (!QueryPerformanceCounter(&epoch)) return 2; }
    while (epoch.QuadPart <= before.QuadPart);
    const unsigned long long oldDelta = before.QuadPart - epoch.QuadPart;
    unsigned long long high{}, remainder{};
    _umul128(oldDelta, 1000000000ull, &high);
    if (high < static_cast<unsigned long long>(frequency.QuadPart)) return 3;
    std::puts("PASS first-sample epoch inversion reproduces division overflow precondition");
    if (!QueryPerformanceCounter(&after)) return 4;
    const unsigned long long delta = after.QuadPart >= epoch.QuadPart ? after.QuadPart - epoch.QuadPart : 0;
    const auto low = _umul128(delta, 1000000000ull, &high);
    const auto ns = _udiv128(high, low, frequency.QuadPart, &remainder);
    if (ns > 1000000000ull) return 5;
    std::puts("PASS epoch-before-sample nanosecond conversion");
    return 0;
}
