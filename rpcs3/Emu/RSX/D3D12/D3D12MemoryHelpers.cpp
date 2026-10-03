#ifdef _MSC_VER
#include "stdafx.h"
#include "stdafx_d3d12.h"
#include "D3D12MemoryHelpers.h"
#include "util/vm.hpp"


void data_cache::store_and_protect_data(u64 key, u32 start, size_t size, u8 format, size_t w, size_t h, size_t d, size_t m, ComPtr<ID3D12Resource> data, u64 layout)
{
	std::lock_guard lock(m_mut);
	m_address_to_data[key] = std::make_pair(texture_entry(format, w, h, d, m, layout), data);
	protect_data_locked(key, start, size);
}

void data_cache::protect_data(u64 key, u32 start, size_t size)
{
	std::lock_guard lock(m_mut);
	protect_data_locked(key, start, size);
}

void data_cache::protect_data_locked(u64 key, u32 start, size_t size)
{
	auto it = m_address_to_data.find(key);
	ensure(it != m_address_to_data.end());
	it->second.first.m_is_dirty = false;
	/// align start to 4096 byte
	static const u32 memory_page_size = 4096;
	if (!size || size > (1ull << 32) - start)
		fmt::throw_exception("Invalid D3D12 texture protection range");
	u32 protected_range_start = start & ~(memory_page_size - 1);
	const u64 range_end = utils::align(static_cast<u64>(start) + size, static_cast<u64>(memory_page_size));
	if (range_end > (1ull << 32)) fmt::throw_exception("Invalid D3D12 texture protection range");
	u32 protected_range_size = ::narrow<u32>(range_end - protected_range_start);
	m_protected_ranges.push_back(std::make_tuple(key, protected_range_start, protected_range_size));
	ensure(pages);
	pages->set(key, protected_range_start, protected_range_size, d3d12::guest_protection::readonly, false);
}

bool data_cache::invalidate_address(u32 addr)
{
    return invalidate_range(addr, 1);
}

bool data_cache::invalidate_range(u32 addr, u32 length)
{
    std::lock_guard lock(m_mut);
    bool handled = false;
    for (const auto& [key, start, size] : m_protected_ranges)
        handled |= length && static_cast<u64>(start) < static_cast<u64>(addr) + length &&
            static_cast<u64>(start) + size > addr;
    if (!handled) return false;
    // Ranges may share pages. Conservatively invalidate ALL cached entries
    // before unprotecting: no overlapping resource can silently remain clean.
    for (auto& [key, entry] : m_address_to_data) entry.first.m_is_dirty = true;
    for (const auto& [key, start, size] : m_protected_ranges)
        pages->remove(key);
    m_protected_ranges.clear();
    return true;
}

std::optional<std::pair<texture_entry, ComPtr<ID3D12Resource>>> data_cache::find_data_if_available(u64 key)
{
    std::lock_guard lock(m_mut);
    const auto it = m_address_to_data.find(key);
    if (it == m_address_to_data.end()) return std::nullopt;
    return it->second; // owning snapshot: never return a map pointer after unlock
}

void data_cache::unprotect_all()
{
	std::lock_guard lock(m_mut);
	for (auto &protectedTexture : m_protected_ranges)
	{
		u32 protectedRangeStart = std::get<1>(protectedTexture), protectedRangeSize = std::get<2>(protectedTexture);
		pages->remove(std::get<0>(protectedTexture));
	}
	m_protected_ranges.clear();
}

ComPtr<ID3D12Resource> data_cache::remove_from_cache(u64 key)
{
	std::lock_guard lock(m_mut);
	auto result = m_address_to_data[key].second;
	m_address_to_data.erase(key);
	return result;
}

void resource_storage::reset()
{
	descriptors_heap_index = 0;
	current_sampler_index = 0;
	sampler_descriptors_heap_index = 0;
	render_targets_descriptors_heap_index = 0;
	depth_stencil_descriptor_heap_index = 0;

	CHECK_HRESULT(command_allocator->Reset());
	set_new_command_list();
}

void resource_storage::set_new_command_list()
{
	CHECK_HRESULT(command_list->Reset(command_allocator.Get(), nullptr));

	ID3D12DescriptorHeap *descriptors[] =
	{
		descriptors_heap.Get(),
		sampler_descriptor_heap[sampler_descriptors_heap_index].Get(),
	};
	command_list->SetDescriptorHeaps(2, descriptors);
}

void resource_storage::init(ID3D12Device *device)
{
	in_use = false;
	m_device = device;
	ram_framebuffer = nullptr;
	// Create a global command allocator
	CHECK_HRESULT(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(command_allocator.GetAddressOf())));

	CHECK_HRESULT(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator.Get(), nullptr, IID_PPV_ARGS(command_list.GetAddressOf())));
	CHECK_HRESULT(command_list->Close());

	D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 50000, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
	CHECK_HRESULT(device->CreateDescriptorHeap(&descriptor_heap_desc, IID_PPV_ARGS(&descriptors_heap)));

	D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER , 2048, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
	CHECK_HRESULT(device->CreateDescriptorHeap(&sampler_heap_desc, IID_PPV_ARGS(&sampler_descriptor_heap[0])));
	CHECK_HRESULT(device->CreateDescriptorHeap(&sampler_heap_desc, IID_PPV_ARGS(&sampler_descriptor_heap[1])));

	D3D12_DESCRIPTOR_HEAP_DESC ds_descriptor_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV , 10000};
	device->CreateDescriptorHeap(&ds_descriptor_heap_desc, IID_PPV_ARGS(&depth_stencil_descriptor_heap));

	D3D12_DESCRIPTOR_HEAP_DESC rtv_descriptor_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV , 10000 };
	device->CreateDescriptorHeap(&rtv_descriptor_heap_desc, IID_PPV_ARGS(&render_targets_descriptors_heap));

	frame_finished_handle = CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
	fence_value = 0;
	CHECK_HRESULT(device->CreateFence(fence_value++, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(frame_finished_fence.GetAddressOf())));
}

void resource_storage::wait_and_clean()
{
	if (in_use)
	{
		while (frame_finished_fence->GetCompletedValue() < fence_value - 1)
		{
			if (WaitForSingleObjectEx(frame_finished_handle, 1000, FALSE) == WAIT_FAILED)
				CHECK_HRESULT(HRESULT_FROM_WIN32(GetLastError()));
			CHECK_HRESULT(m_device->GetDeviceRemovedReason());
		}
		CHECK_HRESULT(m_device->GetDeviceRemovedReason());
	}
	else
		CHECK_HRESULT(command_list->Close());

	reset();

	dirty_textures.clear();
	temporary_passes.clear();

	ram_framebuffer = nullptr;
}

void resource_storage::release()
{
	dirty_textures.clear();
	temporary_passes.clear();
	// NOTE: Should be released only after gfx pipeline last command has been finished.
	CloseHandle(frame_finished_handle);
}

#endif
