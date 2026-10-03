#include "../../Emu/RSX/D3D12/D3D12GuestPages.h"
#include "../../Emu/RSX/D3D12/D3D12CpuAccess.h"
#include <windows.h>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <thread>
#include <array>

// Real Windows page faults and shared guest/sudo mappings. This tests the
// synchronization protocol, not the RPCS3 VM dispatcher or a running game.
struct fault_probe
{
    unsigned char* guest = nullptr;
    unsigned char* sudo = nullptr;
    d3d12::guest_pages pages;
    d3d12::cpu_access_queue access_queue;
    std::mutex mutex;
    std::condition_variable wake;
    bool pending = false;
    bool stopping = false, dirty = true;
    unsigned reads = 0, writes = 0;
    fault_probe(unsigned char* guest_, unsigned char* sudo_) : guest(guest_), sudo(sudo_),
        pages([this](std::uint32_t offset, d3d12::guest_protection mode)
        {
            DWORD old = 0;
            const DWORD protection = mode == d3d12::guest_protection::inaccessible ? PAGE_NOACCESS
                : mode == d3d12::guest_protection::readonly ? PAGE_READONLY : PAGE_READWRITE;
            if (!VirtualProtectFromApp(guest + offset,4096,protection,&old)) throw std::runtime_error("VirtualProtectFromApp failed");
        }) {}
    int fault(EXCEPTION_POINTERS* exception)
    {
        if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
        const auto address = exception->ExceptionRecord->ExceptionInformation[1];
        if (address < reinterpret_cast<ULONG_PTR>(guest) || address >= reinterpret_cast<ULONG_PTR>(guest)+4096)
            return EXCEPTION_CONTINUE_SEARCH;
        access_queue.submit(0,4096,exception->ExceptionRecord->ExceptionInformation[0] == 1,false,[&]{notify();});
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    void notify()
    {
        {std::lock_guard lock(mutex);pending=true;}
        wake.notify_one();
    }
    void run()
    {
        for (;;)
        {
            {
                std::unique_lock lock(mutex);
                wake.wait(lock,[&]{return stopping || pending;});
                if (stopping) return;
                pending=false;
            }
            access_queue.pump([&](auto,auto,bool write)
            {
                // Stand-in for completed GPU readback into the privileged
                // alias. Writing the protected guest mapping here would recurse.
                if (dirty) {for(unsigned i=0;i<4096;++i) sudo[i]=static_cast<unsigned char>(i*37);dirty=false;}
                if (write)
                {
                    ++writes;pages.remove(1);pages.remove(2);
                }
                else {++reads;pages.set(1,0,4096,d3d12::guest_protection::readonly,true);}
            });
        }
    }
};
// Keep C++ objects out of the SEH frames (/EHsc).
static unsigned char read_guest(fault_probe* probe, unsigned offset)
{
    __try {return static_cast<volatile unsigned char*>(probe->guest)[offset];}
    __except(probe->fault(GetExceptionInformation())) {return 0;}
}
static void write_guest(fault_probe* probe, unsigned offset, unsigned char value)
{
    __try {static_cast<volatile unsigned char*>(probe->guest)[offset]=value;}
    __except(probe->fault(GetExceptionInformation())) {}
}
int main()
{
    HANDLE mapping=CreateFileMappingFromApp(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,65536,nullptr);
    if (!mapping) return 1;
    auto* guest=static_cast<unsigned char*>(MapViewOfFileFromApp(mapping,FILE_MAP_ALL_ACCESS,0,65536));
    auto* sudo=static_cast<unsigned char*>(MapViewOfFileFromApp(mapping,FILE_MAP_ALL_ACCESS,0,65536));
    if (!guest || !sudo) return 1;
    int result=0;
    {
        fault_probe probe(guest,sudo);
        std::thread worker([&]{probe.run();});
        try
        {
            probe.pages.set(2,0,4096,d3d12::guest_protection::readonly,false);
            probe.pages.set(1,0,4096,d3d12::guest_protection::inaccessible,true);
            if (read_guest(&probe,7)!=static_cast<unsigned char>(7*37)) throw std::runtime_error("Implicit readback did not retry CPU read");
            write_guest(&probe,7,0x5a);
            if (guest[7]!=0x5a || guest[8]!=static_cast<unsigned char>(8*37)) throw std::runtime_error("Partial CPU write did not preserve GPU bytes");
            // CPU write as the FIRST fault must also read back before retry.
            probe.dirty=true;
            probe.pages.set(1,0,4096,d3d12::guest_protection::inaccessible,true);
            write_guest(&probe,11,0xc3);
            if (guest[11]!=0xc3 || guest[12]!=static_cast<unsigned char>(12*37) || probe.reads!=1 || probe.writes!=2)
                throw std::runtime_error("First-write readback/protection synchronization failed");
            probe.dirty=true;
            probe.pages.set(1,0,4096,d3d12::guest_protection::inaccessible,true);
            if (!probe.access_queue.submit(0,128,true,true,[&]{probe.notify();}))
                throw std::runtime_error("Privileged CPU access lease was not granted");
            // A store via the privileged alias creates no page fault.
            // The same production queue now keeps GPU recording paused until
            // this operation explicitly releases its granted lease.
            sudo[17]=0xb9;
            probe.access_queue.release();
            if (guest[17]!=0xb9 || guest[18]!=static_cast<unsigned char>(18*37))
                throw std::runtime_error("Privileged CPU write lost coherent neighbor bytes");
            std::cout << "PASS: real NOACCESS/read-only CPU faults, worker readback, privileged alias, overlapping texture watch and partial-write retry\n";
            std::cout << "PASS: privileged no-fault CPU store through the production access-lease queue\n";
        }
        catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<"\n";result=1;}
        probe.access_queue.close();
        {std::lock_guard lock(probe.mutex);probe.stopping=true;}
        probe.wake.notify_one();worker.join();probe.pages.clear();
    }
    UnmapViewOfFile(guest);UnmapViewOfFile(sudo);CloseHandle(mapping);
    return result;
}
