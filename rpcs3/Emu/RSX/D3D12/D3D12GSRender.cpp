#ifdef _MSC_VER
#include "stdafx.h"
#include "stdafx_d3d12.h"
#include "D3D12GSRender.h"
#include "D3D12MSAA.h"
#include "util/vm.hpp"
#include "Emu/Memory/vm_locking.h"
#include "../RSXOffload.h"
#include "../Host/MM.h"
#include "../RSXZCULL.h"
#include "Emu/System.h"
#include <wrl/client.h>
#include <dxgi1_4.h>
#include <thread>
#include <chrono>
#include <locale>
#include <codecvt>
#include <cmath>
#include "d3dx12.h"
#include <d3d11on12.h>
#include "D3D12Formats.h"
#include "D3D12Presentation.h"
#include "../rsx_methods.h"

PFN_D3D12_CREATE_DEVICE wrapD3D12CreateDevice;
PFN_D3D12_GET_DEBUG_INTERFACE wrapD3D12GetDebugInterface;
PFN_D3D12_SERIALIZE_ROOT_SIGNATURE wrapD3D12SerializeRootSignature;
PFN_D3D11ON12_CREATE_DEVICE wrapD3D11On12CreateDevice;
pD3DCompile wrapD3DCompile;

ID3D12Device* g_d3d12_device = nullptr;

#define VERTEX_BUFFERS_SLOT 0
#define FRAGMENT_CONSTANT_BUFFERS_SLOT 1
#define VERTEX_CONSTANT_BUFFERS_SLOT 2
#define TEXTURES_SLOT 3
#define SAMPLERS_SLOT 4
#define VERTEX_TEXTURES_SLOT 6
#define VERTEX_SAMPLERS_SLOT 7
#define SCALE_OFFSET_SLOT 5

namespace
{
HMODULE D3D12Module;
HMODULE D3D11Module;
HMODULE D3DCompiler;

void loadD3D12FunctionPointers()
{
#ifdef RPCS3_UWP
	// UWP uses SDK imports; no unrestricted desktop LoadLibrary fallback.
	wrapD3D12CreateDevice = &D3D12CreateDevice;
	wrapD3D12GetDebugInterface = &D3D12GetDebugInterface;
	wrapD3D12SerializeRootSignature = &D3D12SerializeRootSignature;
	wrapD3D11On12CreateDevice = &D3D11On12CreateDevice;
	wrapD3DCompile = &D3DCompile;
#else
	D3D12Module = verify("d3d12.dll", LoadLibrary(L"d3d12.dll"));
	wrapD3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(D3D12Module, "D3D12CreateDevice");
	wrapD3D12GetDebugInterface = (PFN_D3D12_GET_DEBUG_INTERFACE)GetProcAddress(D3D12Module, "D3D12GetDebugInterface");
	wrapD3D12SerializeRootSignature = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(D3D12Module, "D3D12SerializeRootSignature");
	D3D11Module = verify("d3d11.dll", LoadLibrary(L"d3d11.dll"));
	wrapD3D11On12CreateDevice = (PFN_D3D11ON12_CREATE_DEVICE)GetProcAddress(D3D11Module, "D3D11On12CreateDevice");
	D3DCompiler = verify("d3dcompiler_47.dll", LoadLibrary(L"d3dcompiler_47.dll"));
	wrapD3DCompile = (pD3DCompile)GetProcAddress(D3DCompiler, "D3DCompile");
#endif
}

void unloadD3D12FunctionPointers()
{
#ifndef RPCS3_UWP
	FreeLibrary(D3D12Module);
	FreeLibrary(D3D11Module);
	FreeLibrary(D3DCompiler);
#endif
}

/**
 * Wait until command queue has completed all task.
 */
void wait_for_command_queue(ID3D12Device *device, ID3D12CommandQueue *command_queue)
{
	ComPtr<ID3D12Fence> fence;
	CHECK_HRESULT(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
	struct event_owner
	{
		HANDLE handle = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
		~event_owner() { if (handle) CloseHandle(handle); }
	} event;
	if (!event.handle) CHECK_HRESULT(HRESULT_FROM_WIN32(GetLastError()));
	CHECK_HRESULT(fence->SetEventOnCompletion(1, event.handle));
	CHECK_HRESULT(command_queue->Signal(fence.Get(), 1));
	while (fence->GetCompletedValue() < 1)
	{
		if (WaitForSingleObjectEx(event.handle, 1000, FALSE) == WAIT_FAILED)
			CHECK_HRESULT(HRESULT_FROM_WIN32(GetLastError()));
		CHECK_HRESULT(device->GetDeviceRemovedReason());
	}
	CHECK_HRESULT(device->GetDeviceRemovedReason());
}
}

void D3D12GSRender::shader::release()
{
	pso->Release();
	root_signature->Release();
	vertex_buffer->Release();
	texture_descriptor_heap->Release();
	sampler_descriptor_heap->Release();
}

bool D3D12GSRender::invalidate_address(u32 addr)
{
	bool result = false;
	result |= m_texture_cache.invalidate_address(addr);
	return result;
}

D3D12DLLManagement::D3D12DLLManagement()
{
	loadD3D12FunctionPointers();
}

D3D12DLLManagement::~D3D12DLLManagement()
{
	unloadD3D12FunctionPointers();
}

namespace
{
	ComPtr<ID3DBlob> get_shared_root_signature_blob()
	{
		CD3DX12_ROOT_PARAMETER RP[8];

		// vertex buffer are bound each draw calls
		CD3DX12_DESCRIPTOR_RANGE vertex_buffer_descriptors(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		RP[VERTEX_BUFFERS_SLOT].InitAsDescriptorTable(1, &vertex_buffer_descriptors, D3D12_SHADER_VISIBILITY_VERTEX);

		// fragment constants are bound each draw calls
		RP[FRAGMENT_CONSTANT_BUFFERS_SLOT].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);

		// vertex constants are bound often
		CD3DX12_DESCRIPTOR_RANGE vertex_constant_buffer_descriptors(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1);
		RP[VERTEX_CONSTANT_BUFFERS_SLOT].InitAsDescriptorTable(1, &vertex_constant_buffer_descriptors, D3D12_SHADER_VISIBILITY_VERTEX);

		// textures are bound often
		CD3DX12_DESCRIPTOR_RANGE texture_descriptors(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 0);
		RP[TEXTURES_SLOT].InitAsDescriptorTable(1, &texture_descriptors, D3D12_SHADER_VISIBILITY_PIXEL);
		// samplers are bound often
		CD3DX12_DESCRIPTOR_RANGE sampler_descriptors(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16, 0);
		RP[SAMPLERS_SLOT].InitAsDescriptorTable(1, &sampler_descriptors, D3D12_SHADER_VISIBILITY_PIXEL);
		CD3DX12_DESCRIPTOR_RANGE vertex_texture_descriptors(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 16);
		RP[VERTEX_TEXTURES_SLOT].InitAsDescriptorTable(1, &vertex_texture_descriptors, D3D12_SHADER_VISIBILITY_VERTEX);
		CD3DX12_DESCRIPTOR_RANGE vertex_sampler_descriptors(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 4, 0);
		RP[VERTEX_SAMPLERS_SLOT].InitAsDescriptorTable(1, &vertex_sampler_descriptors, D3D12_SHADER_VISIBILITY_VERTEX);

		// scale offset matrix are bound once in a while
		CD3DX12_DESCRIPTOR_RANGE scale_offset_descriptors(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
		RP[SCALE_OFFSET_SLOT].InitAsDescriptorTable(1, &scale_offset_descriptors, D3D12_SHADER_VISIBILITY_ALL);

		Microsoft::WRL::ComPtr<ID3DBlob> root_signature_blob;
		Microsoft::WRL::ComPtr<ID3DBlob> error_blob;
		CHECK_HRESULT(wrapD3D12SerializeRootSignature(
			d3d12::address_of(CD3DX12_ROOT_SIGNATURE_DESC(8, RP, 0, 0)),
			D3D_ROOT_SIGNATURE_VERSION_1, root_signature_blob.GetAddressOf(), error_blob.GetAddressOf()));

		return root_signature_blob;
	}
}

u64 D3D12GSRender::get_cycles()
{
	return thread_ctrl::get_cycles(static_cast<named_thread<D3D12GSRender>&>(*this));
}

D3D12GSRender::D3D12GSRender(utils::serial* ar)
	: GSRender(ar)
	, m_d3d12_lib()
	, m_guest_pages([](u32 page, d3d12::guest_protection protection)
	{
		if (!vm::check_addr(page, vm::page_allocated)) return;
		const auto mode = protection == d3d12::guest_protection::inaccessible || !vm::check_addr(page, vm::page_readable) ? utils::protection::no
			: protection == d3d12::guest_protection::readonly ? utils::protection::ro : utils::protection::rw;
		utils::memory_protect(vm::base(page), 4096, mode == utils::protection::rw && !vm::check_addr(page, vm::page_writable) ? utils::protection::ro : mode);
	})
	, m_current_pso({})
{
	m_texture_cache.pages = &m_guest_pages;
	std::tie(m_device, m_command_queue) = d3d12::presentation().device_and_queue();
	g_d3d12_device = m_device.Get();

	m_descriptor_stride_srv_cbv_uav = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_descriptor_stride_dsv = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	m_descriptor_stride_rtv = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_descriptor_stride_samplers = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

	// No renderer-owned swapchain: immutable offscreen frames go to the host.


	D3D12_DESCRIPTOR_HEAP_DESC current_texture_descriptors_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16 };
	CHECK_HRESULT(m_device->CreateDescriptorHeap(&current_texture_descriptors_desc, IID_PPV_ARGS(m_current_texture_descriptors.GetAddressOf())));
	D3D12_DESCRIPTOR_HEAP_DESC current_sampler_descriptors_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 16 };
	CHECK_HRESULT(m_device->CreateDescriptorHeap(&current_sampler_descriptors_desc, IID_PPV_ARGS(m_current_sampler_descriptors.GetAddressOf())));
	current_texture_descriptors_desc.NumDescriptors = 4;
	current_sampler_descriptors_desc.NumDescriptors = 4;
	CHECK_HRESULT(m_device->CreateDescriptorHeap(&current_texture_descriptors_desc, IID_PPV_ARGS(m_current_vertex_texture_descriptors.GetAddressOf())));
	CHECK_HRESULT(m_device->CreateDescriptorHeap(&current_sampler_descriptors_desc, IID_PPV_ARGS(m_current_vertex_sampler_descriptors.GetAddressOf())));

	ComPtr<ID3DBlob> root_signature_blob = get_shared_root_signature_blob();

	m_device->CreateRootSignature(0,
		root_signature_blob->GetBufferPointer(),
		root_signature_blob->GetBufferSize(),
		IID_PPV_ARGS(m_shared_root_signature.GetAddressOf()));

	m_per_frame_storage[0].init(m_device.Get());
	m_per_frame_storage[0].reset();
	m_per_frame_storage[1].init(m_device.Get());
	m_per_frame_storage[1].reset();

	m_output_scaling_pass.init(m_device.Get(), m_command_queue.Get());

	CHECK_HRESULT(
		m_device->CreateCommittedResource(
			d3d12::address_of(CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT)),
			D3D12_HEAP_FLAG_NONE,
			d3d12::address_of(CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 1)),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_dummy_texture))
			);

	m_rtts.init(m_device.Get());
	m_readback_resources.init(m_device.Get(), 1024 * 1024 * 128, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
	m_buffer_data.init(m_device.Get(), 1024 * 1024 * 896, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

	CHECK_HRESULT(
		m_device->CreateCommittedResource(
			d3d12::address_of(CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT)),
			D3D12_HEAP_FLAG_NONE,
			d3d12::address_of(CD3DX12_RESOURCE_DESC::Buffer(1024 * 1024 * 16)),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(m_vertex_buffer_data.GetAddressOf())
			)
		);

	// The embedded frontend owns overlays. Do not initialize the retired D2D UI.
}

D3D12GSRender::~D3D12GSRender()
{
	if (!m_device)
	{
		//Initialization must have failed
		return;
	}

	wait_for_command_queue(m_device.Get(), m_command_queue.Get());

	m_texture_cache.unprotect_all();
	m_guest_pages.clear();

	m_dummy_texture->Release();
	m_per_frame_storage[0].release();
	m_per_frame_storage[1].release();
	m_output_scaling_pass.release();

	g_d3d12_device = nullptr;
	d3d12::presentation().clear_frame();
}

void D3D12GSRender::on_init_thread()
{
	if (!m_device)
	{
		//Init must have failed
		fmt::throw_exception("No D3D12 device was created");
	}
	backend_config.supports_host_gpu_labels = true;
	backend_config.supports_hw_a2c = true;
}

bool D3D12GSRender::release_GCM_label(u32, u32, u32)
{
	// Complete GPU work before the common processor writes the guest label.
	copy_render_target_to_dma_location();
	auto& storage = get_current_resource_storage();
	CHECK_HRESULT(storage.command_list->Close());
	ID3D12CommandList* commands = storage.command_list.Get();
	m_command_queue->ExecuteCommandLists(1, &commands);
	wait_for_command_queue(m_device.Get(), m_command_queue.Get());
	storage.set_new_command_list();
	return false;
}

void D3D12GSRender::on_semaphore_acquire_wait()
{
	process_surface_requests();
	auto& storage = get_current_resource_storage();
	CHECK_HRESULT(storage.command_list->Close());
	ID3D12CommandList* commands = storage.command_list.Get();
	m_command_queue->ExecuteCommandLists(1, &commands);
	storage.set_new_command_list();
}

void D3D12GSRender::on_exit()
{
	m_cpu_access_queue.close();
	m_guest_pages.clear();
	GSRender::on_exit();
}

void D3D12GSRender::do_local_task(rsx::FIFO::state state)
{
	process_surface_requests();
	apply_surface_invalidations();
	rsx::thread::do_local_task(state);
}

// clear_surface is now dispatched through the current RSX virtual hook.
// Semaphore/writeback integration must use the current framebuffer barriers;
// the removed do_method hook was never invoked by this version of RSX.

void D3D12GSRender::end()
{
	auto& clause = rsx::method_registers.current_draw_clause;
	if (!clause.empty())
	{
		clause.begin();
		bool first = true;
		do
		{
			if (!first) clause.execute_pipeline_dependencies(m_ctx);
			first = false;
			emit_draw();
		} while (clause.next());
	}
	thread::end();
}

void D3D12GSRender::emit_draw()
{
	std::chrono::time_point<steady_clock> start_duration = steady_clock::now();
	prepare_render_targets(get_current_resource_storage().command_list.Get());
	if (!m_graphics_state.test(rsx::pipeline_state::rtt_config_valid)) return;

	std::chrono::time_point<steady_clock> program_load_start = steady_clock::now();
	load_program();
	std::chrono::time_point<steady_clock> program_load_end = steady_clock::now();
	m_timers.program_load_duration += std::chrono::duration_cast<std::chrono::microseconds>(program_load_end - program_load_start).count();

	if (!current_fragment_program.valid)
	{
		return;
	}

	std::chrono::time_point<steady_clock> rtt_duration_start = steady_clock::now();
	prepare_render_targets(get_current_resource_storage().command_list.Get());

	std::chrono::time_point<steady_clock> rtt_duration_end = steady_clock::now();
	m_timers.prepare_rtt_duration += std::chrono::duration_cast<std::chrono::microseconds>(rtt_duration_end - rtt_duration_start).count();

	std::chrono::time_point<steady_clock> vertex_index_duration_start = steady_clock::now();

	size_t currentDescriptorIndex = get_current_resource_storage().descriptors_heap_index;

	size_t vertex_count;
	bool indexed_draw;
	std::vector<D3D12_SHADER_RESOURCE_VIEW_DESC> vertex_buffer_views;
	std::tie(indexed_draw, vertex_count, vertex_buffer_views) = upload_and_set_vertex_index_data(get_current_resource_storage().command_list.Get());
	if (!vertex_count) return;

	UINT vertex_buffer_count = static_cast<UINT>(vertex_buffer_views.size());
	const size_t texture_count = std::get<2>(m_current_pso);
	if (texture_count) upload_textures(get_current_resource_storage().command_list.Get(), texture_count);
	if (current_vp_metadata.referenced_textures_mask)
		upload_vertex_textures(get_current_resource_storage().command_list.Get());

	std::chrono::time_point<steady_clock> vertex_index_duration_end = steady_clock::now();
	m_timers.vertex_index_duration += std::chrono::duration_cast<std::chrono::microseconds>(vertex_index_duration_end - vertex_index_duration_start).count();


	// Switching descriptor heaps invalidates root tables. Switch before binding
	// any tables, and never overwrite descriptors still referenced by the GPU.
	auto& storage = get_current_resource_storage();
	if (storage.current_sampler_index + 20 > 2048)
	{
		ensure(storage.sampler_descriptors_heap_index == 0, "D3D12 per-frame sampler heaps exhausted");
		storage.sampler_descriptors_heap_index = 1;
		storage.current_sampler_index = 0;
		ID3D12DescriptorHeap* heaps[] = {storage.descriptors_heap.Get(), storage.sampler_descriptor_heap[1].Get()};
		storage.command_list->SetDescriptorHeaps(2, heaps);
	}
	ensure(storage.descriptors_heap_index + vertex_buffer_count + 22 <= 50000,
		"D3D12 per-frame resource descriptors exhausted");
	ID3D12DescriptorHeap* draw_heaps[] = {storage.descriptors_heap.Get(),
		storage.sampler_descriptor_heap[storage.sampler_descriptors_heap_index].Get()};
	storage.command_list->SetDescriptorHeaps(2, draw_heaps);
	get_current_resource_storage().command_list->SetGraphicsRootSignature(m_shared_root_signature.Get());
	get_current_resource_storage().command_list->OMSetStencilRef(rsx::method_registers.stencil_func_ref());

	std::chrono::time_point<steady_clock> constants_duration_start = steady_clock::now();

	INT offset = 0;
	for (const auto view : vertex_buffer_views)
	{
		m_device->CreateShaderResourceView(m_vertex_buffer_data.Get(), &view,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(get_current_resource_storage().descriptors_heap->GetCPUDescriptorHandleForHeapStart())
				.Offset((INT)currentDescriptorIndex + offset++, m_descriptor_stride_srv_cbv_uav));
	}
	// Bind vertex buffer
	get_current_resource_storage().command_list->SetGraphicsRootDescriptorTable(VERTEX_BUFFERS_SLOT,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(get_current_resource_storage().descriptors_heap->GetGPUDescriptorHandleForHeapStart())
		.Offset((INT)currentDescriptorIndex, m_descriptor_stride_srv_cbv_uav)
		);

	// Constants
	const D3D12_CONSTANT_BUFFER_VIEW_DESC &fragment_constant_view = upload_fragment_shader_constants();
	get_current_resource_storage().command_list->SetGraphicsRootConstantBufferView(FRAGMENT_CONSTANT_BUFFERS_SLOT, fragment_constant_view.BufferLocation);

	upload_and_bind_scale_offset_matrix(currentDescriptorIndex + vertex_buffer_count);
	get_current_resource_storage().command_list->SetGraphicsRootDescriptorTable(SCALE_OFFSET_SLOT,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(get_current_resource_storage().descriptors_heap->GetGPUDescriptorHandleForHeapStart())
		.Offset((INT)currentDescriptorIndex + vertex_buffer_count, m_descriptor_stride_srv_cbv_uav)
		);

	// Always bind constants after a new command list/root signature. A cached
	// descriptor alone does not restore D3D12 root bindings.
	{
		m_current_transform_constants_buffer_descriptor_id = (u32)currentDescriptorIndex + 1 + vertex_buffer_count;
		upload_and_bind_vertex_shader_constants(currentDescriptorIndex + 1 + vertex_buffer_count);
		get_current_resource_storage().command_list->SetGraphicsRootDescriptorTable(VERTEX_CONSTANT_BUFFERS_SLOT,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(get_current_resource_storage().descriptors_heap->GetGPUDescriptorHandleForHeapStart())
			.Offset(m_current_transform_constants_buffer_descriptor_id, m_descriptor_stride_srv_cbv_uav)
			);
	}

	m_graphics_state &= ~rsx::pipeline_state::memory_barrier_bits;

	std::chrono::time_point<steady_clock> constants_duration_end = steady_clock::now();
	m_timers.constants_duration += std::chrono::duration_cast<std::chrono::microseconds>(constants_duration_end - constants_duration_start).count();

	get_current_resource_storage().command_list->SetPipelineState(std::get<0>(m_current_pso).Get());

	std::chrono::time_point<steady_clock> texture_duration_start = steady_clock::now();

	get_current_resource_storage().descriptors_heap_index += 2 + vertex_buffer_count;
	if (texture_count > 0)
	{

		m_device->CopyDescriptorsSimple(static_cast<UINT>(texture_count),
			CD3DX12_CPU_DESCRIPTOR_HANDLE(get_current_resource_storage().descriptors_heap->GetCPUDescriptorHandleForHeapStart())
				.Offset((UINT)get_current_resource_storage().descriptors_heap_index, m_descriptor_stride_srv_cbv_uav),
			CD3DX12_CPU_DESCRIPTOR_HANDLE(m_current_texture_descriptors->GetCPUDescriptorHandleForHeapStart()),
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
			);

		m_device->CopyDescriptorsSimple(static_cast<UINT>(texture_count),
			CD3DX12_CPU_DESCRIPTOR_HANDLE(get_current_resource_storage().sampler_descriptor_heap[get_current_resource_storage().sampler_descriptors_heap_index]->GetCPUDescriptorHandleForHeapStart())
				.Offset((UINT)get_current_resource_storage().current_sampler_index, m_descriptor_stride_samplers),
			CD3DX12_CPU_DESCRIPTOR_HANDLE(m_current_sampler_descriptors->GetCPUDescriptorHandleForHeapStart()),
			D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
			);

		get_current_resource_storage().command_list->SetGraphicsRootDescriptorTable(TEXTURES_SLOT,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(get_current_resource_storage().descriptors_heap->GetGPUDescriptorHandleForHeapStart())
			.Offset((INT)get_current_resource_storage().descriptors_heap_index, m_descriptor_stride_srv_cbv_uav)
			);
		get_current_resource_storage().command_list->SetGraphicsRootDescriptorTable(SAMPLERS_SLOT,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(get_current_resource_storage().sampler_descriptor_heap[get_current_resource_storage().sampler_descriptors_heap_index]->GetGPUDescriptorHandleForHeapStart())
			.Offset((INT)get_current_resource_storage().current_sampler_index, m_descriptor_stride_samplers)
			);

		get_current_resource_storage().current_sampler_index += 16;
		get_current_resource_storage().descriptors_heap_index += 16;
	}

	if (current_vp_metadata.referenced_textures_mask)
	{
		auto& storage = get_current_resource_storage();
		m_device->CopyDescriptorsSimple(4,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(storage.descriptors_heap->GetCPUDescriptorHandleForHeapStart(),
				static_cast<INT>(storage.descriptors_heap_index), m_descriptor_stride_srv_cbv_uav),
			m_current_vertex_texture_descriptors->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		m_device->CopyDescriptorsSimple(4,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(storage.sampler_descriptor_heap[storage.sampler_descriptors_heap_index]->GetCPUDescriptorHandleForHeapStart(),
				static_cast<INT>(storage.current_sampler_index), m_descriptor_stride_samplers),
			m_current_vertex_sampler_descriptors->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
		storage.command_list->SetGraphicsRootDescriptorTable(VERTEX_TEXTURES_SLOT,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(storage.descriptors_heap->GetGPUDescriptorHandleForHeapStart(),
				static_cast<INT>(storage.descriptors_heap_index), m_descriptor_stride_srv_cbv_uav));
		storage.command_list->SetGraphicsRootDescriptorTable(VERTEX_SAMPLERS_SLOT,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(storage.sampler_descriptor_heap[storage.sampler_descriptors_heap_index]->GetGPUDescriptorHandleForHeapStart(),
				static_cast<INT>(storage.current_sampler_index), m_descriptor_stride_samplers));
		storage.descriptors_heap_index += 4;
		storage.current_sampler_index += 4;
	}

	std::chrono::time_point<steady_clock> texture_duration_end = steady_clock::now();
	m_timers.texture_duration += std::chrono::duration_cast<std::chrono::microseconds>(texture_duration_end - texture_duration_start).count();
	set_rtt_and_ds(get_current_resource_storage().command_list.Get());

	int clip_w = rsx::method_registers.surface_clip_width();
	int clip_h = rsx::method_registers.surface_clip_height();

	D3D12_VIEWPORT viewport =
	{
		0.f,
		0.f,
		(float)clip_w,
		(float)clip_h,
		0.f,
		1.f,
	};
	get_current_resource_storage().command_list->RSSetViewports(1, &viewport);

	get_current_resource_storage().command_list->RSSetScissorRects(1, d3d12::address_of(::get_scissor(rsx::method_registers.scissor_origin_x(), rsx::method_registers.scissor_origin_y(),
		rsx::method_registers.scissor_width(), rsx::method_registers.scissor_height())));

	get_current_resource_storage().command_list->IASetPrimitiveTopology(get_primitive_topology(rsx::method_registers.current_draw_clause.primitive));

	if (indexed_draw)
	{
		watch_bound_surfaces();
		get_current_resource_storage().command_list->DrawIndexedInstanced((UINT)vertex_count, 1, 0, 0, 0);
	}
	else
	{
		watch_bound_surfaces();
		get_current_resource_storage().command_list->DrawInstanced((UINT)vertex_count, 1, 0, 0);
	}

	std::chrono::time_point<steady_clock> end_duration = steady_clock::now();
	m_timers.draw_calls_duration += std::chrono::duration_cast<std::chrono::microseconds>(end_duration - start_duration).count();
	m_timers.draw_calls_count++;

	if (g_cfg.video.debug_output)
	{
		CHECK_HRESULT(get_current_resource_storage().command_list->Close());
		m_command_queue->ExecuteCommandLists(1, (ID3D12CommandList**)get_current_resource_storage().command_list.GetAddressOf());
		get_current_resource_storage().set_new_command_list();
	}
}

namespace
{
bool is_flip_surface_in_global_memory(rsx::surface_target color_target)
{
	switch (color_target)
	{
	case rsx::surface_target::surface_a:
	case rsx::surface_target::surface_b:
	case rsx::surface_target::surfaces_a_b:
	case rsx::surface_target::surfaces_a_b_c:
	case rsx::surface_target::surfaces_a_b_c_d:
		return true;
	case rsx::surface_target::none:
		return false;
	}
	fmt::throw_exception("Wrong color_target");
}
}

void D3D12GSRender::flip(const rsx::display_flip_info_t& info)
{
    if (info.skip_frame) { GSRender::flip(info); return; }
    ensure(info.buffer < std::size(display_buffers));
    const auto& display = display_buffers[info.buffer];
    const u32 address = rsx::get_address(display.offset, CELL_GCM_LOCATION_LOCAL);
    ID3D12Resource* source = m_rtts.get_texture_from_render_target_if_applicable(address);
    auto& storage = get_current_resource_storage();
    auto* commands = storage.command_list.Get();
    bool bound = false;
    for (const auto& target : m_rtts.m_bound_render_targets)
        bound |= target.second && target.second == source;
    bool ram_source = false;
    if (!source && display.width && display.height)
    {
        const usz row_bytes = static_cast<usz>(display.width) * 4;
        ensure(display.pitch >= row_bytes);
        ensure(vm::check_addr(address, vm::page_readable,
            ::narrow<u32>(static_cast<usz>(display.pitch) * display.height)));
        const usz pitch = utils::align(row_bytes, 256u);
        const usz bytes = pitch * display.height;
        const usz offset = m_buffer_data.alloc<512>(bytes);
        auto* mapped = m_buffer_data.map<std::byte>(offset);
        for (u32 row = 0; row < display.height; ++row)
            std::memcpy(mapped + row * pitch, static_cast<const std::byte*>(vm::base(address)) + row * display.pitch, row_bytes);
        m_buffer_data.unmap(CD3DX12_RANGE(offset, offset + bytes));
        CHECK_HRESULT(m_device->CreateCommittedResource(
            d3d12::address_of(CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT)), D3D12_HEAP_FLAG_NONE,
            d3d12::address_of(CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, display.width, display.height)),
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(storage.ram_framebuffer.GetAddressOf())));
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{offset, {DXGI_FORMAT_R8G8B8A8_UNORM, display.width, display.height, 1, ::narrow<UINT>(pitch)}};
        commands->CopyTextureRegion(d3d12::address_of(CD3DX12_TEXTURE_COPY_LOCATION(storage.ram_framebuffer.Get(), 0)),
            0, 0, 0, d3d12::address_of(CD3DX12_TEXTURE_COPY_LOCATION(m_buffer_data.get_heap(), footprint)), nullptr);
        commands->ResourceBarrier(1, d3d12::address_of(CD3DX12_RESOURCE_BARRIER::Transition(storage.ram_framebuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ)));
        source = storage.ram_framebuffer.Get();
        ram_source = true;
    }
    if (!source) { GSRender::flip(info); return; }
    auto* original_source = source;
    if (bound)
        commands->ResourceBarrier(1, d3d12::address_of(CD3DX12_RESOURCE_BARRIER::Transition(source,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ)));
    ComPtr<ID3D12Resource> output;
    if (source->GetDesc().SampleDesc.Count > 1)
    {
        auto resolved = d3d12::resolve_color(m_device.Get(), commands, source, D3D12_RESOURCE_STATE_GENERIC_READ);
        source = resolved.Get();
        storage.dirty_textures.push_back(std::move(resolved));
    }
    const UINT width = display.width, height = display.height;
    ensure(width && height);
    CHECK_HRESULT(m_device->CreateCommittedResource(
        d3d12::address_of(CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT)), D3D12_HEAP_FLAG_NONE,
        d3d12::address_of(CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)),
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(output.GetAddressOf())));
    if (!m_backbuffer_descriptor_heap[m_frame_index])
    {
        const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
        CHECK_HRESULT(m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(m_backbuffer_descriptor_heap[m_frame_index].GetAddressOf())));
    }
    const auto rtv = m_backbuffer_descriptor_heap[m_frame_index]->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(output.Get(), nullptr, rtv);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = source->GetDesc().Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    srv.Shader4ComponentMapping = ram_source ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1, 2, 3, 0)
        : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    m_device->CreateShaderResourceView(source, &srv,
        CD3DX12_CPU_DESCRIPTOR_HANDLE(m_output_scaling_pass.texture_descriptor_heap->GetCPUDescriptorHandleForHeapStart()).Offset(m_frame_index, m_descriptor_stride_srv_cbv_uav));
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    m_device->CreateSampler(&sampler,
        CD3DX12_CPU_DESCRIPTOR_HANDLE(m_output_scaling_pass.sampler_descriptor_heap->GetCPUDescriptorHandleForHeapStart()).Offset(m_frame_index, m_descriptor_stride_samplers));
    ID3D12DescriptorHeap* heaps[]{m_output_scaling_pass.texture_descriptor_heap, m_output_scaling_pass.sampler_descriptor_heap};
    commands->SetDescriptorHeaps(2, heaps);
    commands->SetGraphicsRootSignature(m_output_scaling_pass.root_signature);
    commands->SetPipelineState(m_output_scaling_pass.pso);
    commands->SetGraphicsRootDescriptorTable(0,
        CD3DX12_GPU_DESCRIPTOR_HANDLE(heaps[0]->GetGPUDescriptorHandleForHeapStart()).Offset(m_frame_index, m_descriptor_stride_srv_cbv_uav));
    commands->SetGraphicsRootDescriptorTable(1,
        CD3DX12_GPU_DESCRIPTOR_HANDLE(heaps[1]->GetGPUDescriptorHandleForHeapStart()).Offset(m_frame_index, m_descriptor_stride_samplers));
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
    const D3D12_VERTEX_BUFFER_VIEW vertices{m_output_scaling_pass.vertex_buffer->GetGPUVirtualAddress(), 16 * sizeof(float), 4 * sizeof(float)};
    commands->IASetVertexBuffers(0, 1, &vertices);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    commands->DrawInstanced(4, 1, 0, 0);
    commands->ResourceBarrier(1, d3d12::address_of(CD3DX12_RESOURCE_BARRIER::Transition(output.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)));
    if (bound)
        commands->ResourceBarrier(1, d3d12::address_of(CD3DX12_RESOURCE_BARRIER::Transition(original_source,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET)));
    CHECK_HRESULT(commands->Close());
    ID3D12CommandList* list = commands;
    m_command_queue->ExecuteCommandLists(1, &list);
    storage.dirty_textures.push_back(output); // retain through producer GPU completion
    d3d12::presentation().publish(output.Get());
    CHECK_HRESULT(m_command_queue->Signal(storage.frame_finished_fence.Get(), storage.fence_value));
    CHECK_HRESULT(storage.frame_finished_fence->SetEventOnCompletion(storage.fence_value++, storage.frame_finished_handle));
    storage.in_use = true;
    storage.dirty_textures.splice(storage.dirty_textures.end(), m_rtts.invalidated_resources);
    storage.buffer_heap_get_pos = m_buffer_data.get_current_put_pos_minus_one();
    storage.readback_heap_get_pos = m_readback_resources.get_current_put_pos_minus_one();
    m_frame_index ^= 1;
    auto& next = get_current_resource_storage();
    const bool reclaim = next.in_use;
    next.wait_and_clean();
    if (reclaim)
    {
        m_buffer_data.set_get_pos(next.buffer_heap_get_pos);
        m_readback_resources.set_get_pos(next.readback_heap_get_pos);
    }
    reset_timer();
    GSRender::flip(info);
}

bool D3D12GSRender::on_access_violation(u32 address, bool is_writing)
{
	// Match Vulkan/OpenGL: finish pending RSX memory-manager writes before
	// deciding which cache/report controller owns this fault.
	rsx::mm_flush(address);
	if (!m_guest_pages.surface_hits(address, 1).empty())
	{
		request_surface_memory(address & ~4095u, 4096, is_writing);
		if (is_writing) invalidate_address(address);
		return true;
	}
	if (!is_writing)
	{
		return zcull_ctrl->on_access_violation(address);
	}

	if (invalidate_address(address))
	{
		{
			std::lock_guard lock(m_surface_invalidation_mutex);
			m_surface_invalidations.emplace_back(address & ~4095u, 4096);
		}
		rsx_log.warning("Reporting Cell writing to 0x%x", address);
		return true;
	}

	return zcull_ctrl->on_access_violation(address);
}

void D3D12GSRender::apply_surface_invalidations()
{
	std::vector<std::pair<u32, u32>> ranges;
	{
		std::lock_guard lock(m_surface_invalidation_mutex);
		ranges.swap(m_surface_invalidations);
	}
	for (const auto& [address, length] : ranges)
		if (m_rtts.invalidate_memory_range(address, length))
		{
			m_graphics_state |= rsx::pipeline_state::rtt_config_dirty;
			m_graphics_state |= rsx::pipeline_state::vertex_program_state_dirty;
			m_graphics_state |= rsx::pipeline_state::fragment_program_state_dirty;
		}
}

void D3D12GSRender::write_barrier(u32 address, u32 range)
{
	ensure(is_current_thread());
	synchronize_surface_memory(address, range, true);
	{
		std::lock_guard lock(m_surface_invalidation_mutex);
		m_surface_invalidations.emplace_back(address, range);
	}
	apply_surface_invalidations();
}

rsx::flags32_t D3D12GSRender::read_barrier(u32 address, u32 range, bool unconditional)
{
	request_surface_memory(address, range, false);
	return thread::read_barrier(address, range, unconditional);
}

void D3D12GSRender::on_invalidate_memory_range(const utils::address_range32& range, rsx::invalidation_cause cause)
{
	// RSX calls this under its engine/task exclusion, including from the
	// unmapping CPU when the renderer has been stopped for a capture.
	if (cause != rsx::invalidation_cause::unmap)
	{
		synchronize_surface_memory(range.start, range.length(), false);
		return;
	}
	for (u64 key : m_guest_pages.surface_hits(range.start, range.length()))
	{
		if (const auto found = m_watched_surfaces.find(key); found != m_watched_surfaces.end())
		{
			const auto desc = found->second.resource->GetDesc();
			const u64 size = u64(found->second.pitch) * desc.Height * (desc.SampleDesc.Count > 1 ? desc.SampleDesc.Count / 2 : 1);
			if (m_rtts.invalidate_memory_range(found->second.address, static_cast<u32>(size)))
				m_graphics_state |= rsx::pipeline_state::rtt_config_dirty | rsx::pipeline_state::vertex_program_state_dirty | rsx::pipeline_state::fragment_program_state_dirty;
		}
		m_guest_pages.remove(key);
		m_watched_surfaces.erase(key);
	}
	m_texture_cache.invalidate_range(range.start, range.length());
	if (m_rtts.invalidate_memory_range(range.start, range.length()))
		m_graphics_state |= rsx::pipeline_state::rtt_config_dirty | rsx::pipeline_state::vertex_program_state_dirty | rsx::pipeline_state::fragment_program_state_dirty;
}

bool D3D12GSRender::begin_cpu_memory_access(u32 address, u32 length)
{
	// There can be no GPU-owned guest surfaces before renderer startup.
	if (!rsx_thread_running) return false;
	return request_surface_memory(address, length, true, true);
}

void D3D12GSRender::end_cpu_memory_access() noexcept
{
	m_cpu_access_queue.release();
}

bool D3D12GSRender::request_surface_memory(u32 address, u32 length, bool write, bool lease)
{
	if (!length || (!lease && m_guest_pages.surface_hits(address, length).empty())) return false;
	if (is_current_thread()) { synchronize_surface_memory(address, length, write); return false; }
	// The faulting guest CPU must not retain the VM lock while waiting for RSX.
	vm::temporary_unlock();
	auto& offloader = g_fxo->get<rsx::dma_manager>();
	const bool offloader_fault = offloader.is_current_thread();
	if (offloader_fault) offloader.set_mem_fault_flag();
	struct offloader_recovery
	{
		rsx::dma_manager& manager;
		bool active;
		~offloader_recovery() { if (active) manager.clear_mem_fault_flag(); }
	} recovery{offloader, offloader_fault};
	return m_cpu_access_queue.submit(address, length, write, lease, [&]
	{
		m_eng_interrupt_mask |= rsx::backend_interrupt;
		thread_ctrl::notify(static_cast<named_thread<D3D12GSRender>&>(*this));
	});
}

void D3D12GSRender::process_surface_requests()
{
	m_cpu_access_queue.pump([&](u32 address, u32 length, bool write)
	{
		synchronize_surface_memory(address, length, write);
	});
}

void D3D12GSRender::watch_bound_surfaces(bool dirty)
{
	d3d12::set_sample_positions(m_device.Get(), get_current_resource_storage().command_list.Get(), m_active_sample_pattern);
	const auto watch = [&](u32 address, ID3D12Resource* image, u32 pitch, bool depth)
	{
		if (!image) return;
		const auto desc = image->GetDesc();
		const u64 rows = u64(desc.Height) * (desc.SampleDesc.Count > 1 ? desc.SampleDesc.Count / 2 : 1);
		const u64 size = u64(pitch) * rows;
		ensure(size && size <= UINT32_MAX && vm::check_addr(address, vm::page_writable, static_cast<u32>(size)));
		const u64 key = (1ull << 63) | reinterpret_cast<u64>(image);
		if (get_tiled_memory_region(utils::address_range32::start_length(address, static_cast<u32>(size))))
			fmt::throw_exception("D3D12 guest surface tiling/coherence is not implemented");
		for (const auto& [other_key, other] : m_watched_surfaces)
		{
			if (other_key == key) continue;
			const auto other_desc = other.resource->GetDesc();
			const u64 other_size = u64(other.pitch) * other_desc.Height * (other_desc.SampleDesc.Count > 1 ? other_desc.SampleDesc.Count / 2 : 1);
			if (u64(other.address) < u64(address) + size && u64(address) < u64(other.address) + other_size)
				fmt::throw_exception("D3D12 simultaneous overlapping guest surface attachments are unsupported");
		}
		m_watched_surfaces[key] = {image, address, pitch, m_framebuffer_layout.color_format,
			m_framebuffer_layout.depth_format, depth, dirty};
		m_guest_pages.set(key, address, size, dirty ? d3d12::guest_protection::inaccessible : d3d12::guest_protection::readonly, true);
	};
	for (u32 i = 0; i < 4; ++i)
		watch(m_rtts.m_bound_render_targets[i].first, m_rtts.m_bound_render_targets[i].second,
			m_framebuffer_layout.actual_color_pitch[i], false);
	watch(m_rtts.m_bound_depth_stencil.first, m_rtts.m_bound_depth_stencil.second,
		m_framebuffer_layout.actual_zeta_pitch, true);
}

void D3D12GSRender::synchronize_surface_memory(u32 address, u32 length, bool write)
{
	ensure(is_current_thread() || is_stopped());
	const auto keys = m_guest_pages.surface_hits(address, length);
	for (const u64 key : keys)
	{
		auto found = m_watched_surfaces.find(key);
		if (found == m_watched_surfaces.end()) continue;
		auto& record = found->second;
		const auto desc = record.resource->GetDesc();
		const u32 rows = desc.Height * (desc.SampleDesc.Count > 1 ? desc.SampleDesc.Count / 2 : 1);
		if (record.dirty)
		{
			bool bound = m_rtts.m_bound_depth_stencil.second == record.resource.Get();
			for (const auto& attachment : m_rtts.m_bound_render_targets) bound |= attachment.second == record.resource.Get();
			const auto state = bound ? (record.is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET)
				: D3D12_RESOURCE_STATE_GENERIC_READ;
			std::vector<std::byte> bytes;
			if (record.is_depth)
			{
				auto planes = rsx::download_depth_surface(record.resource.Get(), record.depth, m_device.Get(),
					m_command_queue.Get(), m_readback_resources, get_current_resource_storage(), state, m_active_sample_pattern);
				if (!planes[1].empty())
					for (usz i = 0; i < planes[1].size(); ++i) planes[0][i * 4 + 3] = planes[1][i];
				bytes = std::move(planes[0]);
			}
			else bytes = rsx::download_color_surface(record.resource.Get(), record.color, m_device.Get(),
				m_command_queue.Get(), m_readback_resources, get_current_resource_storage(), state, m_active_sample_pattern);
			ensure(rows && bytes.size() % rows == 0);
			const usz row_bytes = bytes.size() / rows;
			ensure(row_bytes <= record.pitch);
			ensure(vm::check_addr(record.address, vm::page_writable, static_cast<u32>(u64(record.pitch) * rows)));
			auto* destination = vm::get_super_ptr<std::byte>(record.address);
			for (u32 row = 0; row < rows; ++row)
				std::memcpy(destination + usz(row) * record.pitch, bytes.data() + usz(row) * row_bytes, row_bytes);
			m_texture_cache.invalidate_range(record.address, static_cast<u32>(u64(record.pitch) * rows));
			record.dirty = false;
		}
		if (write)
		{
			if (m_rtts.invalidate_memory_range(record.address, static_cast<u32>(u64(record.pitch) * rows)))
				m_graphics_state |= rsx::pipeline_state::rtt_config_dirty | rsx::pipeline_state::vertex_program_state_dirty | rsx::pipeline_state::fragment_program_state_dirty;
			m_guest_pages.remove(key);
			m_watched_surfaces.erase(found);
		}
		else m_guest_pages.set(key, record.address, u64(record.pitch) * rows, d3d12::guest_protection::readonly, true);
	}
	// Invalidate even when no render target overlaps: guest writes can target
	// texture-only ranges. Keep renderer-local and queued CPU access identical.
	if (write) m_texture_cache.invalidate_range(address, length);
}

void D3D12GSRender::reset_timer()
{
	m_timers.draw_calls_count = 0;
	m_timers.draw_calls_duration = 0;
	m_timers.prepare_rtt_duration = 0;
	m_timers.vertex_index_duration = 0;
	m_timers.buffer_upload_size = 0;
	m_timers.program_load_duration = 0;
	m_timers.constants_duration = 0;
	m_timers.texture_duration = 0;
	m_timers.flip_duration = 0;
}

resource_storage& D3D12GSRender::get_current_resource_storage()
{
return m_per_frame_storage[m_frame_index];
}

resource_storage& D3D12GSRender::get_non_current_resource_storage()
{
return m_per_frame_storage[1 - m_frame_index];
}
#endif
