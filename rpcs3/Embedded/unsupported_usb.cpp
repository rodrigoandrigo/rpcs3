#include "stdafx.h"
#include "Emu/Cell/lv2/sys_usbd.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/IdManager.h"
#include "uwp_usb_bus.h"
LOG_CHANNEL(sys_usbd);

namespace
{
    struct uwp_usb_service : uwp_usb_bus
    {
        shared_mutex mutex;
        ppu_thread* waiters = nullptr;

        void deliver(event value)
        {
            if (auto cpu = lv2_obj::schedule<ppu_thread>(waiters, SYS_SYNC_PRIORITY))
            {
                cpu->gpr[4] = value[0];
                cpu->gpr[5] = value[1];
                cpu->gpr[6] = value[2];
                lv2_obj::awake(cpu);
            }
        }
    };

    error_code register_driver(ppu_thread& ppu, u32 handle, vm::cptr<char> product,
        u16 length, uwp_usb_bus::driver ids)
    {
        ppu.state += cpu_flag::wait;
        if (!product || !length || !vm::check_addr(product.addr(), vm::page_readable, length))
            return CELL_EFAULT;
        auto& bus = g_fxo->get<uwp_usb_service>();
        std::lock_guard lock(bus.mutex);
        if (!bus.valid(handle)) return CELL_EINVAL;
        return bus.register_driver(std::string(product.get_ptr(), length), ids) ? CELL_OK : CELL_EEXIST;
    }
}

// The firmware implements MIO requests, replies, shared memory and RSX audio.
// Allow its server to initialize on an empty USB bus instead of aborting with
// ENOSYS before it creates the MIO IPC queue. Physical USB remains unsupported.
error_code sys_usbd_initialize(ppu_thread& ppu, vm::ptr<u32> handle)
{
    ppu.state += cpu_flag::wait;
    if (!handle || !vm::check_addr(handle.addr(), vm::page_writable, sizeof(u32))) return CELL_EFAULT;
    auto& bus = g_fxo->get<uwp_usb_service>();
    {
        std::lock_guard lock(bus.mutex);
        if (!bus.initialize()) return CELL_EEXIST;
    }
    *handle = uwp_usb_bus::handle;
    return CELL_OK;
}

error_code sys_usbd_finalize(ppu_thread& ppu, u32 handle)
{
    ppu.state += cpu_flag::wait;
    auto& bus = g_fxo->get<uwp_usb_service>();
    std::lock_guard lock(bus.mutex);
    if (!bus.valid(handle)) return CELL_EINVAL;
    bus.finalize();
    while (bus.waiters) bus.deliver({SYS_USBD_TERMINATE, 0, 0});
    return CELL_OK;
}

error_code sys_usbd_get_device_list(ppu_thread& ppu, u32 handle, vm::ptr<UsbInternalDevice> device_list, u32 max_devices)
{
    ppu.state += cpu_flag::wait;
    auto& bus = g_fxo->get<uwp_usb_service>();
    std::lock_guard lock(bus.mutex);
    if (!bus.valid(handle)) return CELL_EINVAL;
    // No physical USB backend: zero devices, with no guest buffer writes.
    return not_an_error(0);
}

error_code sys_usbd_get_descriptor_size(ppu_thread& ppu, u32 handle, u32 device_handle)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_get_descriptor_size"); return CELL_ENOSYS; }

error_code sys_usbd_get_descriptor(ppu_thread& ppu, u32 handle, u32 device_handle, vm::ptr<void> descriptor, u32 desc_size)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_get_descriptor"); return CELL_ENOSYS; }

error_code sys_usbd_register_ldd(ppu_thread& ppu, u32 handle, vm::cptr<char> s_product, u16 slen_product)
{ return register_driver(ppu, handle, s_product, slen_product, {}); }

error_code sys_usbd_unregister_ldd(ppu_thread& ppu, u32 handle, vm::cptr<char> s_product, u16 slen_product)
{ return sys_usbd_unregister_extra_ldd(ppu, handle, s_product, slen_product); }

error_code sys_usbd_open_pipe(ppu_thread& ppu, u32 handle, u32 device_handle, u32 unk1, u64 unk2, u64 unk3, u32 endpoint, u64 unk4)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_open_pipe"); return CELL_ENOSYS; }

error_code sys_usbd_open_default_pipe(ppu_thread& ppu, u32 handle, u32 device_handle)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_open_default_pipe"); return CELL_ENOSYS; }

error_code sys_usbd_close_pipe(ppu_thread& ppu, u32 handle, u32 pipe_handle)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_close_pipe"); return CELL_ENOSYS; }

error_code sys_usbd_receive_event(ppu_thread& ppu, u32 handle, vm::ptr<u64> arg1, vm::ptr<u64> arg2, vm::ptr<u64> arg3)
{
    ppu.state += cpu_flag::wait;
    if (!arg1 || !arg2 || !arg3 ||
        !vm::check_addr(arg1.addr(), vm::page_writable, sizeof(u64)) ||
        !vm::check_addr(arg2.addr(), vm::page_writable, sizeof(u64)) ||
        !vm::check_addr(arg3.addr(), vm::page_writable, sizeof(u64))) return CELL_EFAULT;

    auto& bus = g_fxo->get<uwp_usb_service>();
    {
        std::lock_guard lock(bus.mutex);
        if (!bus.valid(handle)) return CELL_EINVAL;
        if (!bus.events.empty())
        {
            const auto value = bus.events.front();
            bus.events.pop_front();
            *arg1 = value[0]; *arg2 = value[1]; *arg3 = value[2];
            return CELL_OK;
        }
        // A stopped syscall can be retried; do not enqueue its waiter twice.
        bool queued = false;
        for (auto cpu = bus.waiters; cpu; cpu = cpu->next_cpu) queued |= cpu == &ppu;
        lv2_obj::sleep(ppu);
        if (!queued) lv2_obj::emplace(bus.waiters, &ppu);
    }
    while (auto state = +ppu.state)
    {
        if (state & cpu_flag::signal && ppu.state.test_and_reset(cpu_flag::signal)) break;
        if (is_stopped(state))
        {
            std::lock_guard lock(bus.mutex);
            for (auto cpu = bus.waiters; cpu; cpu = cpu->next_cpu)
            {
                if (cpu == &ppu)
                {
                    ppu.state += cpu_flag::again;
                    return {};
                }
            }
            break;
        }
        ppu.state.wait(state);
    }
    ppu.check_state();
    *arg1 = ppu.gpr[4]; *arg2 = ppu.gpr[5]; *arg3 = ppu.gpr[6];
    return CELL_OK;
}

error_code sys_usbd_detect_event(ppu_thread& ppu)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_detect_event"); return CELL_ENOSYS; }

error_code sys_usbd_attach(ppu_thread& ppu, u32 handle, u32 unk1, u32 unk2, u32 device_handle)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_attach"); return CELL_ENOSYS; }

error_code sys_usbd_transfer_data(ppu_thread& ppu, u32 handle, u32 id_pipe, vm::ptr<u8> buf, u32 buf_size, vm::ptr<UsbDeviceRequest> request, u32 type_transfer)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_transfer_data"); return CELL_ENOSYS; }

error_code sys_usbd_isochronous_transfer_data(ppu_thread& ppu, u32 handle, u32 id_pipe, vm::ptr<UsbDeviceIsoRequest> iso_request)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_isochronous_transfer_data"); return CELL_ENOSYS; }

error_code sys_usbd_get_transfer_status(ppu_thread& ppu, u32 handle, u32 id_transfer, u32 unk1, vm::ptr<u32> result, vm::ptr<u32> count)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_get_transfer_status"); return CELL_ENOSYS; }

error_code sys_usbd_get_isochronous_transfer_status(ppu_thread& ppu, u32 handle, u32 id_transfer, u32 unk1, vm::ptr<UsbDeviceIsoRequest> request, vm::ptr<u32> result)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_get_isochronous_transfer_status"); return CELL_ENOSYS; }

error_code sys_usbd_get_device_location(ppu_thread& ppu, u32 handle, u32 device_handle, vm::ptr<u8> location)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_get_device_location"); return CELL_ENOSYS; }

error_code sys_usbd_send_event(ppu_thread& ppu)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_send_event"); return CELL_ENOSYS; }

error_code sys_usbd_event_port_send(ppu_thread& ppu, u32 handle, u64 arg1, u64 arg2, u64 arg3)
{
    ppu.state += cpu_flag::wait;
    auto& bus = g_fxo->get<uwp_usb_service>();
    std::lock_guard lock(bus.mutex);
    if (!bus.valid(handle)) return CELL_EINVAL;
    if (bus.waiters) bus.deliver({arg1, arg2, arg3});
    else if (!bus.push({arg1, arg2, arg3})) return CELL_EBUSY;
    return CELL_OK;
}

error_code sys_usbd_allocate_memory(ppu_thread& ppu)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_allocate_memory"); return CELL_ENOSYS; }

error_code sys_usbd_free_memory(ppu_thread& ppu)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_free_memory"); return CELL_ENOSYS; }

error_code sys_usbd_get_device_speed(ppu_thread& ppu)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_get_device_speed"); return CELL_ENOSYS; }

error_code sys_usbd_register_extra_ldd(ppu_thread& ppu, u32 handle, vm::cptr<char> s_product, u16 slen_product, u16 id_vendor, u16 id_product_min, u16 id_product_max)
{
    if (id_product_min > id_product_max) return CELL_EINVAL;
    return register_driver(ppu, handle, s_product, slen_product, {id_vendor, id_product_min, id_product_max});
}

error_code sys_usbd_unregister_extra_ldd(ppu_thread& ppu, u32 handle, vm::cptr<char> s_product, u16 slen_product)
{
    ppu.state += cpu_flag::wait;
    if (!s_product || !slen_product || !vm::check_addr(s_product.addr(), vm::page_readable, slen_product))
        return CELL_EFAULT;
    auto& bus = g_fxo->get<uwp_usb_service>();
    std::lock_guard lock(bus.mutex);
    if (!bus.valid(handle)) return CELL_EINVAL;
    return bus.drivers.erase(std::string(s_product.get_ptr(), slen_product)) ? CELL_OK : CELL_ESRCH;
}

void connect_usb_controller(u8, input::product_type) {}
void reconnect_usb(u32) {}
void set_usb_device_attached(usb_device*, bool) {}
void handle_hotplug_event(bool, bool) {}

