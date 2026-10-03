#include "../../Emu/RSX/D3D12/D3D12CpuAccess.h"
#include <atomic>
#include <chrono>
#include <iostream>
using namespace std::chrono_literals;
int main()
{
    for (unsigned iteration=0;iteration<100;++iteration)
    {
        d3d12::cpu_access_queue queue;
        std::promise<void> pending;
        std::atomic<unsigned> synchronized=0;
        auto first=std::async(std::launch::async,[&]{return queue.submit(0x1000,128,true,true,[&]{pending.set_value();});});
        pending.get_future().get();
        auto gpu=std::async(std::launch::async,[&]{queue.pump([&](auto,auto,auto){++synchronized;});});
        if (!first.get() || synchronized!=1 || gpu.wait_for(0ms)==std::future_status::ready) return 1;
        // Another grant while the first lease is alive must not deadlock.
        if (!queue.submit(0x2000,128,true,true,[]{})) return 1;
        queue.release();
        if (synchronized!=2 || gpu.wait_for(0ms)==std::future_status::ready) return 1;
        queue.release();gpu.get();queue.close();
    }
    d3d12::cpu_access_queue stopped;
    std::promise<void> pending;
    auto abandoned=std::async(std::launch::async,[&]
    {
        try {stopped.submit(0,128,true,true,[&]{pending.set_value();});return false;}
        catch (const std::runtime_error&) {return true;}
    });
    pending.get_future().get();stopped.close();
    if (!abandoned.get()) return 1;
    d3d12::cpu_access_queue failing;
    std::promise<void> failed_pending;
    auto failed=std::async(std::launch::async,[&]
    {
        try {failing.submit(0,128,true,true,[&]{failed_pending.set_value();});return false;}
        catch(const std::runtime_error&) {return true;}
    });
    failed_pending.get_future().get();
    failing.pump([](auto,auto,auto){throw std::runtime_error("GPU readback failed");});
    if (!failed.get()) return 1;
    bool overflow=false;
    try {failing.submit(0xffffffff,128,true,true,[]{});} catch(const std::invalid_argument&) {overflow=true;}
    if (!overflow || failing.submit(0,0,true,true,[]{})) return 1;
    failing.close();
    std::cout<<"PASS: 100 CPU/GPU lease handoffs, nested grants, full-scope exclusion, shutdown, failure propagation and range guards\n";
}
