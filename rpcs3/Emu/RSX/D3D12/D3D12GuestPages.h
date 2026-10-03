#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace d3d12
{
enum class guest_protection : unsigned { writable, readonly, inaccessible };

// One arbiter for surface and texture protection. Removing a texture watch
// cannot make a dirty GPU surface readable/writable through an overlapping page.
class guest_pages
{
    struct watch { std::uint64_t begin, end; guest_protection protection; bool surface; };
    std::mutex mutex;
    std::unordered_map<std::uint64_t, watch> watches;
    std::function<void(std::uint32_t, guest_protection)> apply;
    void refresh(std::uint64_t begin, std::uint64_t end)
    {
        for (auto page = begin; page < end; page += 4096)
        {
            auto protection = guest_protection::writable;
            for (const auto& [key, entry] : watches)
                if (entry.begin < page + 4096 && entry.end > page &&
                    unsigned(entry.protection) > unsigned(protection)) protection = entry.protection;
            apply(static_cast<std::uint32_t>(page), protection);
        }
    }
public:
    explicit guest_pages(std::function<void(std::uint32_t, guest_protection)> callback) : apply(std::move(callback)) {}
    void set(std::uint64_t key, std::uint32_t address, std::uint64_t size, guest_protection protection, bool surface)
    {
        if (!size || size > (1ull << 32) - address) throw std::invalid_argument("Invalid guest protection range");
        const auto begin = std::uint64_t(address & ~4095u);
        const auto end = (std::uint64_t(address) + size + 4095) & ~4095ull;
        std::lock_guard lock(mutex);
        auto old = watches.find(key);
        std::uint64_t old_begin = begin, old_end = end;
        if (old != watches.end()) { old_begin = old->second.begin; old_end = old->second.end; }
        watches[key] = {begin, end, protection, surface};
        refresh(old_begin, old_end);
        if (begin != old_begin || end != old_end) refresh(begin, end);
    }
    void remove(std::uint64_t key)
    {
        std::lock_guard lock(mutex);
        const auto found = watches.find(key);
        if (found == watches.end()) return;
        const auto range = found->second;
        watches.erase(found);
        refresh(range.begin, range.end);
    }
    std::vector<std::uint64_t> surface_hits(std::uint32_t address, std::uint32_t size)
    {
        std::lock_guard lock(mutex);
        std::vector<std::uint64_t> result;
        const auto end = std::uint64_t(address) + size;
        for (const auto& [key, entry] : watches)
            if (entry.surface && size && entry.begin < end && entry.end > address) result.push_back(key);
        return result;
    }
    void clear()
    {
        std::lock_guard lock(mutex);
        auto old = std::move(watches);
        watches.clear();
        for (const auto& [key, entry] : old) refresh(entry.begin, entry.end);
    }
};
}
