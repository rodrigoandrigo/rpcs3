#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

// Guest USB control plane only. No devices, descriptors or completed transfers
// are fabricated. The firmware's sys_audio server uses this control plane even
// when no USB audio peripheral is attached.
struct uwp_usb_bus
{
    static constexpr std::uint32_t handle = 0x115b;
    static constexpr std::size_t event_limit = 256;
    using event = std::array<std::uint64_t, 3>;
    using driver = std::array<std::uint16_t, 3>;

    bool initialized = false;
    std::map<std::string, driver> drivers;
    std::deque<event> events;

    bool valid(std::uint32_t value) const
    {
        return initialized && value == handle;
    }

    bool initialize()
    {
        if (initialized) return false;
        initialized = true;
        return true;
    }

    void finalize()
    {
        initialized = false;
        drivers.clear();
        events.clear();
    }

    bool register_driver(std::string name, driver ids)
    {
        return drivers.emplace(std::move(name), ids).second;
    }

    bool push(event value)
    {
        if (events.size() >= event_limit) return false;
        events.push_back(value);
        return true;
    }
};
