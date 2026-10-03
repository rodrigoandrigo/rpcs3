#pragma once
#include <cstdint>
#include <utility>

namespace d3d12
{
// Renderer-thread only. Move invalidated resources to fence-owned retirement
// storage; never release images which earlier command lists can still reference.
template <typename Images, typename Sizes, typename Retired, typename Callback>
bool invalidate_surface_memory(Images& images, Sizes& sizes, Retired& retired,
    std::uint32_t address, std::uint32_t length, Callback&& detach)
{
    if (!length) return false;
    const std::uint64_t end = std::uint64_t(address) + length;
    bool changed = false;
    for (auto it = images.begin(); it != images.end();)
    {
        const std::uint64_t begin = it->first;
        if (begin < end && begin + sizes.at(it->first) > address)
        {
            detach(it->second);
            sizes.erase(it->first);
            retired.push_back(std::move(it->second));
            it = images.erase(it);
            changed = true;
        }
        else ++it;
    }
    return changed;
}
}
