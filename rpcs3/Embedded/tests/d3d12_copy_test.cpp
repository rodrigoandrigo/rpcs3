#include "Emu/RSX/D3D12/D3D12BufferCopy.h"
#include "Emu/RSX/D3D12/D3D12SurfaceRanges.h"
#include "Emu/RSX/D3D12/D3D12GuestPages.h"
#include <array>
#include <cstdio>
#include <unordered_map>
#include <list>
#include <memory>

int main()
{
    alignas(16) std::array<unsigned char, 544> source{}, destination{};
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<unsigned char>(i * 37);
    for (auto copy : {streamToBuffer, streamBuffer})
    {
        for (size_t length = 0; length <= 512; ++length)
        {
            for (size_t alignment = 0; alignment < 16; ++alignment)
            {
                destination.fill(0xcd);
                copy(destination.data() + alignment, source.data() + 1, length);
                for (size_t i = 0; i < destination.size(); ++i)
                {
                    const auto expected = i >= alignment && i < alignment + length
                        ? source[i - alignment + 1] : 0xcd;
                    if (destination[i] != expected) return 1;
                }
            }
        }
    }
    std::puts("PASS: D3D12 streaming copies, 0..512-byte tails, unaligned input/output and guards");
    std::unordered_map<std::uint32_t, std::shared_ptr<unsigned>> images;
    std::unordered_map<std::uint32_t, std::uint64_t> sizes;
    std::list<std::shared_ptr<unsigned>> retired;
    images[0x1000] = std::make_shared<unsigned>(1);
    images[0x2000] = std::make_shared<unsigned>(2);
    images[0xfffff000] = std::make_shared<unsigned>(3);
    sizes[0x1000] = sizes[0x2000] = sizes[0xfffff000] = 0x1000;
    unsigned bound = 2, detached = 0;
    const auto detach = [&](const auto& image) { if (*image == bound) bound = 0; ++detached; };
    if (d3d12::invalidate_surface_memory(images, sizes, retired, 0x2000, 0, detach)) return 1;
    if (!d3d12::invalidate_surface_memory(images, sizes, retired, 0x2000, 1, detach)) return 1;
    if (images.size() != 2 || sizes.size() != 2 || retired.size() != 1 || bound != 0 || detached != 1) return 1;
    if (d3d12::invalidate_surface_memory(images, sizes, retired, 0x3000, 1, detach)) return 1;
    if (!d3d12::invalidate_surface_memory(images, sizes, retired, 0xffffffff, 1, detach)) return 1;
    if (images.size() != 1 || retired.size() != 2 || detached != 2 || *retired.front() != 2) return 1;
    if (!d3d12::invalidate_surface_memory(images, sizes, retired, 0x1fff, 2, detach)) return 1;
    if (!images.empty() || !sizes.empty() || retired.size() != 3 || detached != 3) return 1;
    std::puts("PASS: surface range overlap, exclusive endpoints, zero length, 4GiB boundary, detachment and fence-owned retirement");
    using protection = d3d12::guest_protection;
    std::unordered_map<std::uint32_t, protection> protections;
    d3d12::guest_pages pages([&](std::uint32_t page, protection value) {protections[page] = value;});
    pages.set(1,0x1010,32,protection::inaccessible,true);
    pages.set(2,0x1080,4096,protection::readonly,false);
    if (protections[0x1000] != protection::inaccessible || protections[0x2000] != protection::readonly) return 1;
    // Removing an overlapping texture may not expose the dirty GPU surface.
    pages.remove(2);
    if (protections[0x1000] != protection::inaccessible || protections[0x2000] != protection::writable) return 1;
    if (pages.surface_hits(0x1fff,1).size()!=1 || !pages.surface_hits(0x2000,1).empty()) return 1;
    pages.set(1,0x1010,32,protection::readonly,true);
    if (protections[0x1000]!=protection::readonly) return 1;
    pages.set(3,0x10a0,32,protection::inaccessible,true);
    pages.remove(1);
    if (protections[0x1000]!=protection::inaccessible) return 1;
    pages.set(3,0xffffffff,1,protection::readonly,true);
    if (protections[0x1000]!=protection::writable || protections[0xfffff000]!=protection::readonly) return 1;
    if (pages.surface_hits(0xffffffff,1).size()!=1 || !pages.surface_hits(0xffffffff,0).empty()) return 1;
    bool rejected = false;
    try {pages.set(4,0xffffffff,2,protection::readonly,true);} catch(const std::invalid_argument&) {rejected=true;}
    if (!rejected) return 1;
    pages.clear();
    if (protections[0xfffff000]!=protection::writable || !pages.surface_hits(0xffffffff,1).empty()) return 1;
    std::puts("PASS: shared surface/texture page protection, readback downgrade, overlapping owners, replacement, 4GiB boundary and teardown");
    return 0;
}
