#include "../uwp_usb_bus.h"
#include <iostream>
#include <stdexcept>

static void check(bool value)
{
    if (!value) throw std::runtime_error("USB control plane assertion failed");
}

int main() try
{
    uwp_usb_bus bus;
    check(!bus.valid(uwp_usb_bus::handle));
    check(bus.initialize());
    check(!bus.initialize());
    check(bus.valid(uwp_usb_bus::handle));
    check(!bus.valid(0));
    check(bus.register_driver("audio", {0x54c, 1, 2}));
    check(!bus.register_driver("audio", {}));
    check(bus.drivers.at("audio")[0] == 0x54c);
    check(bus.drivers.erase("audio") == 1);
    check(bus.drivers.erase("audio") == 0);
    for (std::uint64_t i = 0; i < bus.event_limit; i++) check(bus.push({i, i + 1, i + 2}));
    check(!bus.push({999, 0, 0}));
    for (std::uint64_t i = 0; i < bus.event_limit; i++)
    {
        check(bus.events.front() == uwp_usb_bus::event{i, i + 1, i + 2});
        bus.events.pop_front();
    }
    check(bus.push({4, 0, 0}));
    bus.finalize();
    check(!bus.initialized && bus.events.empty() && bus.drivers.empty());
    check(bus.initialize());
    std::cout << "PASS: empty USB bus lifecycle, driver registry, bounded FIFO and reset\n";
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
