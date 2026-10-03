#pragma once
#include "util/types.hpp"

namespace rsx
{
class thread;
// No-op for backends which do not implement privileged-access leases.
// Declare before VM/reservation locks so it is destroyed after those locks.
class cpu_memory_access
{
    thread* renderer = nullptr;
public:
    cpu_memory_access() = default;
    cpu_memory_access(u32 address, u32 length);
    cpu_memory_access(const cpu_memory_access&) = delete;
    cpu_memory_access& operator=(const cpu_memory_access&) = delete;
    ~cpu_memory_access();
    bool active() const {return renderer != nullptr;}
    void reset(u32 address, u32 length);
};
}
