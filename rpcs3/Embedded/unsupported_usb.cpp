#include "stdafx.h"
#include "Emu/Cell/lv2/sys_usbd.h"
LOG_CHANNEL(sys_usbd);
// No host USB library, no fake successful guest device operations.
error_code sys_usbd_initialize(ppu_thread& ppu, vm::ptr<u32> handle)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_initialize"); return CELL_ENOSYS; }

error_code sys_usbd_finalize(ppu_thread& ppu, u32 handle)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_finalize"); return CELL_ENOSYS; }

error_code sys_usbd_get_device_list(ppu_thread& ppu, u32 handle, vm::ptr<UsbInternalDevice> device_list, u32 max_devices)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_get_device_list"); return CELL_ENOSYS; }

error_code sys_usbd_get_descriptor_size(ppu_thread& ppu, u32 handle, u32 device_handle)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_get_descriptor_size"); return CELL_ENOSYS; }

error_code sys_usbd_get_descriptor(ppu_thread& ppu, u32 handle, u32 device_handle, vm::ptr<void> descriptor, u32 desc_size)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_get_descriptor"); return CELL_ENOSYS; }

error_code sys_usbd_register_ldd(ppu_thread& ppu, u32 handle, vm::cptr<char> s_product, u16 slen_product)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_register_ldd"); return CELL_ENOSYS; }

error_code sys_usbd_unregister_ldd(ppu_thread& ppu, u32 handle, vm::cptr<char> s_product, u16 slen_product)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_unregister_ldd"); return CELL_ENOSYS; }

error_code sys_usbd_open_pipe(ppu_thread& ppu, u32 handle, u32 device_handle, u32 unk1, u64 unk2, u64 unk3, u32 endpoint, u64 unk4)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_open_pipe"); return CELL_ENOSYS; }

error_code sys_usbd_open_default_pipe(ppu_thread& ppu, u32 handle, u32 device_handle)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_open_default_pipe"); return CELL_ENOSYS; }

error_code sys_usbd_close_pipe(ppu_thread& ppu, u32 handle, u32 pipe_handle)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_close_pipe"); return CELL_ENOSYS; }

error_code sys_usbd_receive_event(ppu_thread& ppu, u32 handle, vm::ptr<u64> arg1, vm::ptr<u64> arg2, vm::ptr<u64> arg3)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_receive_event"); return CELL_ENOSYS; }

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
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_event_port_send"); return CELL_ENOSYS; }

error_code sys_usbd_allocate_memory(ppu_thread& ppu)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_allocate_memory"); return CELL_ENOSYS; }

error_code sys_usbd_free_memory(ppu_thread& ppu)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_free_memory"); return CELL_ENOSYS; }

error_code sys_usbd_get_device_speed(ppu_thread& ppu)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_get_device_speed"); return CELL_ENOSYS; }

error_code sys_usbd_register_extra_ldd(ppu_thread& ppu, u32 handle, vm::cptr<char> s_product, u16 slen_product, u16 id_vendor, u16 id_product_min, u16 id_product_max)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_register_extra_ldd"); return CELL_ENOSYS; }

error_code sys_usbd_unregister_extra_ldd(ppu_thread& ppu, u32 handle, vm::cptr<char> s_product, u16 slen_product)
{ sys_usbd.warning("USB unavailable in UWP: sys_usbd_unregister_extra_ldd"); return CELL_ENOSYS; }

void connect_usb_controller(u8, input::product_type) {}
void reconnect_usb(u32) {}
void set_usb_device_attached(usb_device*, bool) {}
void handle_hotplug_event(bool, bool) {}

