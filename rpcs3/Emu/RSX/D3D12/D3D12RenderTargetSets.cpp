#ifdef _MSC_VER
#include "stdafx.h"
#include "stdafx_d3d12.h"
#include "D3D12RenderTargetSets.h"
#include "Emu/Memory/vm.h"
#include "Emu/System.h"
#include "Emu/RSX/GSRender.h"
#include "../rsx_methods.h"

#include <D3D12.h>
#include "D3D12GSRender.h"
#include "D3D12Formats.h"
#include "D3D12Readback.h"
#include "D3D12MSAA.h"
#include "D3D12SamplePositions.h"
#include "D3D12SurfaceRestore.h"
#include "../Common/TextureUtils.h"

namespace
{
	u32 get_max_depth_value(rsx::surface_depth_format2 format)
	{
		switch (format)
		{
		case rsx::surface_depth_format2::z16_uint:
		case rsx::surface_depth_format2::z16_float: return 0xFFFF;
		case rsx::surface_depth_format2::z24s8_uint:
		case rsx::surface_depth_format2::z24s8_float: return 0xFFFFFF;
		}
		fmt::throw_exception("Unknown depth format");
	}

	UINT get_num_rtt(rsx::surface_target color_target)
	{
		switch (color_target)
		{
		case rsx::surface_target::none: return 0;
		case rsx::surface_target::surface_a:
		case rsx::surface_target::surface_b: return 1;
		case rsx::surface_target::surfaces_a_b: return 2;
		case rsx::surface_target::surfaces_a_b_c: return 3;
		case rsx::surface_target::surfaces_a_b_c_d: return 4;
		}
		fmt::throw_exception("Unknown color target");
	}

	std::vector<u8> get_rtt_indexes(rsx::surface_target color_target)
	{
		switch (color_target)
		{
		case rsx::surface_target::none: return{};
		case rsx::surface_target::surface_a: return{ 0 };
		case rsx::surface_target::surface_b: return{ 1 };
		case rsx::surface_target::surfaces_a_b: return{ 0, 1 };
		case rsx::surface_target::surfaces_a_b_c: return{ 0, 1, 2 };
		case rsx::surface_target::surfaces_a_b_c_d: return{ 0, 1, 2, 3 };
		}
		fmt::throw_exception("Unknown color target");
	}

	u8 get_clear_stencil(u32 register_value)
	{
		return register_value & 0xff;
	}

	size_t get_aligned_pitch(rsx::surface_color_format format, u32 width)
	{
		switch (format)
		{
		case rsx::surface_color_format::b8: return utils::align(width, 256);
		case rsx::surface_color_format::g8b8:
		case rsx::surface_color_format::x1r5g5b5_o1r5g5b5:
		case rsx::surface_color_format::x1r5g5b5_z1r5g5b5:
		case rsx::surface_color_format::r5g6b5: return utils::align(width * 2, 256);
		case rsx::surface_color_format::a8b8g8r8:
		case rsx::surface_color_format::x8b8g8r8_o8b8g8r8:
		case rsx::surface_color_format::x8b8g8r8_z8b8g8r8:
		case rsx::surface_color_format::x8r8g8b8_o8r8g8b8:
		case rsx::surface_color_format::x8r8g8b8_z8r8g8b8:
		case rsx::surface_color_format::x32:
		case rsx::surface_color_format::a8r8g8b8: return utils::align(width * 4, 256);
		case rsx::surface_color_format::w16z16y16x16: return utils::align(width * 8, 256);
		case rsx::surface_color_format::w32z32y32x32: return utils::align(width * 16, 256);
		}
		fmt::throw_exception("Unknown color surface format");
	}

	size_t get_packed_pitch(rsx::surface_color_format format, u32 width)
	{
		switch (format)
		{
		case rsx::surface_color_format::b8: return width;
		case rsx::surface_color_format::g8b8:
		case rsx::surface_color_format::x1r5g5b5_o1r5g5b5:
		case rsx::surface_color_format::x1r5g5b5_z1r5g5b5:
		case rsx::surface_color_format::r5g6b5: return width * 2;
		case rsx::surface_color_format::a8b8g8r8:
		case rsx::surface_color_format::x8b8g8r8_o8b8g8r8:
		case rsx::surface_color_format::x8b8g8r8_z8b8g8r8:
		case rsx::surface_color_format::x8r8g8b8_o8r8g8b8:
		case rsx::surface_color_format::x8r8g8b8_z8r8g8b8:
		case rsx::surface_color_format::x32:
		case rsx::surface_color_format::a8r8g8b8: return width * 4;
		case rsx::surface_color_format::w16z16y16x16: return width * 8;
		case rsx::surface_color_format::w32z32y32x32: return width * 16;
		}
		fmt::throw_exception("Unknown color surface format");
	}
}

std::vector<u8> rsx::utility::get_rtt_indexes(surface_target target)
{
	return ::get_rtt_indexes(target);
}

void D3D12GSRender::clear_surface(u32 arg)
{
	if ((arg & 0xf3) == 0) return;

	std::chrono::time_point<steady_clock> start_duration = steady_clock::now();

	std::chrono::time_point<steady_clock> rtt_duration_start = steady_clock::now();
	prepare_render_targets(get_current_resource_storage().command_list.Get());

	if (!m_graphics_state.test(rsx::pipeline_state::rtt_config_valid)) return;
	watch_bound_surfaces();

	std::chrono::time_point<steady_clock> rtt_duration_end = steady_clock::now();
	m_timers.prepare_rtt_duration += std::chrono::duration_cast<std::chrono::microseconds>(rtt_duration_end - rtt_duration_start).count();

	if ((arg & 0x3) && m_rtts.m_bound_depth_stencil.second)
	{
		get_current_resource_storage().depth_stencil_descriptor_heap_index++;

		if (arg & 0x1)
		{
			auto depth_format = rsx::method_registers.surface_depth_fmt();
			u32 clear_depth = rsx::method_registers.z_clear_value(depth_format == rsx::surface_depth_format::z24s8);
			u32 max_depth_value = rsx::get_max_depth_value(depth_format);
			get_current_resource_storage().command_list->ClearDepthStencilView(m_rtts.current_ds_handle, D3D12_CLEAR_FLAG_DEPTH, clear_depth / (float)max_depth_value, 0,
				1, d3d12::address_of(::get_scissor(rsx::method_registers.scissor_origin_x(), rsx::method_registers.scissor_origin_y(), rsx::method_registers.scissor_width(), rsx::method_registers.scissor_height())));
		}

		if ((arg & 0x2) && rsx::method_registers.surface_depth_fmt() == rsx::surface_depth_format::z24s8)
			get_current_resource_storage().command_list->ClearDepthStencilView(m_rtts.current_ds_handle, D3D12_CLEAR_FLAG_STENCIL, 0.f, get_clear_stencil(rsx::method_registers.stencil_clear_value()),
				1, d3d12::address_of(::get_scissor(rsx::method_registers.scissor_origin_x(), rsx::method_registers.scissor_origin_y(), rsx::method_registers.scissor_width(), rsx::method_registers.scissor_height())));
	}

	if (arg & 0xF0)
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtts.current_rtts_handle);
		size_t rtt_index = m_rtts.m_bound_render_targets_config.second;
		get_current_resource_storage().render_targets_descriptors_heap_index += rtt_index;
		std::array<float, 4> clear_color =
		{
			rsx::method_registers.clear_color_r() / 255.f,
			rsx::method_registers.clear_color_g() / 255.f,
			rsx::method_registers.clear_color_b() / 255.f,
			rsx::method_registers.clear_color_a() / 255.f,
		};
		for (unsigned i = 0; i < rtt_index; i++)
			get_current_resource_storage().command_list->ClearRenderTargetView(CD3DX12_CPU_DESCRIPTOR_HANDLE(handle).Offset(::narrow<INT>(i), m_descriptor_stride_rtv), clear_color.data(),
				1, d3d12::address_of(::get_scissor(rsx::method_registers.scissor_origin_x(), rsx::method_registers.scissor_origin_y(), rsx::method_registers.scissor_width(), rsx::method_registers.scissor_height())));
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

void D3D12GSRender::prepare_render_targets(ID3D12GraphicsCommandList *copycmdlist)
{
	apply_surface_invalidations();
	// Exit early if there is no rtt changes
	if (!m_graphics_state.test(rsx::pipeline_state::rtt_config_dirty))
		return;
	m_graphics_state.clear(rsx::pipeline_state::rtt_config_dirty);

	std::array<float, 4> clear_color =
	{
		rsx::method_registers.clear_color_r() / 255.f,
		rsx::method_registers.clear_color_g() / 255.f,
		rsx::method_registers.clear_color_b() / 255.f,
		rsx::method_registers.clear_color_a() / 255.f,
	};

	rsx::framebuffer_layout layout;
	get_framebuffer_layout(rsx::framebuffer_creation_context::context_draw, layout);
	if (!m_graphics_state.test(rsx::pipeline_state::rtt_config_valid))
		return;
	if (layout.raster_type != rsx::surface_raster_type::linear)
		fmt::throw_exception("D3D12 surface coherence requires a linear guest raster layout");

	// Conservative alias policy: commit old surfaces before changing the
	// framebuffer layout, then restore replacements from coherent guest RAM.
	// This avoids unordered old/new GPU aliases overwriting one another.
	while (!m_watched_surfaces.empty())
	{
		const auto address = m_watched_surfaces.begin()->second.address;
		synchronize_surface_memory(address, 1, true);
	}
	m_graphics_state.clear(rsx::pipeline_state::rtt_config_dirty);
	switch (layout.aa_mode)
	{
	case rsx::surface_antialiasing::center_1_sample: m_active_sample_pattern = d3d12::sample_pattern::center; break;
	case rsx::surface_antialiasing::diagonal_centered_2_samples: m_active_sample_pattern = d3d12::sample_pattern::diagonal; break;
	case rsx::surface_antialiasing::square_centered_4_samples: m_active_sample_pattern = d3d12::sample_pattern::square; break;
	case rsx::surface_antialiasing::square_rotated_4_samples: m_active_sample_pattern = d3d12::sample_pattern::rotated; break;
	}
	d3d12::set_sample_positions(m_device.Get(), copycmdlist, m_active_sample_pattern);

	m_rtts.prepare_render_target(copycmdlist,
		layout.color_format, layout.depth_format,
		layout.width, layout.height,
		layout.target, layout.aa_mode,
		layout.color_addresses, layout.zeta_address,
		layout.actual_color_pitch, layout.actual_zeta_pitch,
		m_device.Get(), clear_color, 1.f, 0);
	on_framebuffer_layout_updated();
	watch_bound_surfaces(false);
	restore_bound_surfaces();

	// write descriptors
	DXGI_FORMAT dxgi_format = get_color_surface_format(rsx::method_registers.surface_color());
	D3D12_RENDER_TARGET_VIEW_DESC rtt_view_desc = {};
	rtt_view_desc.ViewDimension = rsx::d3d12_sample_count(layout.aa_mode) > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
	rtt_view_desc.Format = dxgi_format;

	m_rtts.current_rtts_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(get_current_resource_storage().render_targets_descriptors_heap->GetCPUDescriptorHandleForHeapStart())
		.Offset((INT)get_current_resource_storage().render_targets_descriptors_heap_index * m_descriptor_stride_rtv);
	size_t rtt_index = 0;
	for (u8 i : get_rtt_indexes(rsx::method_registers.surface_color_target()))
	{
		if (std::get<1>(m_rtts.m_bound_render_targets[i]) == nullptr)
			continue;
		m_device->CreateRenderTargetView(std::get<1>(m_rtts.m_bound_render_targets[i]), &rtt_view_desc,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtts.current_rtts_handle).Offset((INT)rtt_index * m_descriptor_stride_rtv));
		rtt_index++;
	}
	get_current_resource_storage().render_targets_descriptors_heap_index += rtt_index;

	if (std::get<1>(m_rtts.m_bound_depth_stencil) == nullptr)
		return;
	m_rtts.current_ds_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(get_current_resource_storage().depth_stencil_descriptor_heap->GetCPUDescriptorHandleForHeapStart())
		.Offset((INT)get_current_resource_storage().depth_stencil_descriptor_heap_index * m_descriptor_stride_dsv);
	get_current_resource_storage().depth_stencil_descriptor_heap_index += 1;
	D3D12_DEPTH_STENCIL_VIEW_DESC depth_stencil_view_desc = {};
	depth_stencil_view_desc.Format = get_depth_stencil_surface_format(rsx::method_registers.surface_depth_fmt());
	depth_stencil_view_desc.ViewDimension = rsx::d3d12_sample_count(layout.aa_mode) > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
	m_device->CreateDepthStencilView(std::get<1>(m_rtts.m_bound_depth_stencil), &depth_stencil_view_desc, m_rtts.current_ds_handle);
}

void D3D12GSRender::restore_bound_surfaces()
{
	for (const auto& [key, record] : m_watched_surfaces)
	{
		const auto desc = record.resource->GetDesc();
		const u32 sx = desc.SampleDesc.Count > 1 ? 2 : 1;
		const u32 sy = desc.SampleDesc.Count > 1 ? desc.SampleDesc.Count / 2 : 1;
		const usz width = desc.Width * sx, rows = desc.Height * sy;
		const usz pixel_size = record.is_depth
			? (record.depth == rsx::surface_depth_format2::z16_uint || record.depth == rsx::surface_depth_format2::z16_float ? 2 : 4)
			: rsx::get_format_block_size_in_bytes(record.color);
		ensure(width * pixel_size <= record.pitch);
		std::vector<std::byte> guest(width * rows * pixel_size);
		const auto* source = vm::get_super_ptr<std::byte>(record.address);
		for (usz y = 0; y < rows; ++y)
			std::memcpy(guest.data() + y * width * pixel_size, source + y * record.pitch, width * pixel_size);
		std::vector<std::byte> pixels, stencil;
		if (!record.is_depth)
		{
			const usz component = record.color == rsx::surface_color_format::w16z16y16x16 ? 2
				: record.color == rsx::surface_color_format::w32z32y32x32 ? 4 : pixel_size;
			for (usz i = 0; i < guest.size(); i += component)
				std::reverse(guest.begin() + i, guest.begin() + i + component);
			pixels = std::move(guest);
		}
		else
		{
			pixels.resize(width * rows * 4);
			if (pixel_size == 4) stencil.resize(width * rows);
			for (usz i = 0; i < width * rows; ++i)
			{
				u32 value = (u32(std::to_integer<u8>(guest[i * pixel_size])) << 8) | std::to_integer<u8>(guest[i * pixel_size + 1]);
				float depth;
				if (pixel_size == 4)
				{
					value = (value << 8) | std::to_integer<u8>(guest[i * 4 + 2]);
					stencil[i] = guest[i * 4 + 3];
					if (record.depth == rsx::surface_depth_format2::z24s8_float)
					{ const u32 bits = value << 7; std::memcpy(&depth, &bits, 4); }
					else depth = float(double(value) / 16777215.);
				}
				else if (record.depth == rsx::surface_depth_format2::z16_float)
				{ const u32 bits = (value << 11) + (120u << 23); std::memcpy(&depth, &bits, 4); }
				else depth = float(value) / 65535.f;
				std::memcpy(pixels.data() + i * 4, &depth, 4);
			}
		}
		auto restored = d3d12::restore_surface(m_device.Get(), get_current_resource_storage().command_list.Get(),
			record.resource.Get(), record.is_depth ? get_depth_stencil_surface_format(record.depth) : get_color_surface_format(record.color), pixels, stencil);
		get_current_resource_storage().temporary_passes.push_back(std::move(restored));
	}
}

void D3D12GSRender::set_rtt_and_ds(ID3D12GraphicsCommandList *command_list)
{
	UINT num_rtt = m_rtts.m_bound_render_targets_config.second;
	D3D12_CPU_DESCRIPTOR_HANDLE* ds_handle = (std::get<1>(m_rtts.m_bound_depth_stencil) != nullptr) ? &m_rtts.current_ds_handle : nullptr;
	command_list->OMSetRenderTargets((UINT)num_rtt, &m_rtts.current_rtts_handle, true, ds_handle);
}

void rsx::render_targets::init(ID3D12Device *device)
{
	g_descriptor_stride_rtv = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}


namespace
{
    d3d12::readback_plane download(ID3D12Resource* surface, ID3D12Device* device,
        ID3D12CommandQueue* queue, resource_storage& storage, D3D12_RESOURCE_STATES state, UINT plane, d3d12::sample_pattern pattern)
    {
        if (surface->GetDesc().SampleDesc.Count > 1)
        {
            const bool depth = (surface->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
            auto samples = d3d12::expand_samples(device, storage.command_list.Get(), surface, state, depth, plane, nullptr, pattern);
            auto result = d3d12::download_plane(device, queue, storage.command_list.Get(), samples->image.Get(), D3D12_RESOURCE_STATE_GENERIC_READ, 0);
            storage.set_new_command_list();
            return result;
        }
        auto result = d3d12::download_plane(device, queue, storage.command_list.Get(), surface, state, plane);
        storage.set_new_command_list();
        return result;
    }
    void reverse_components(std::vector<std::byte>& bytes, usz size)
    {
        ensure(size && bytes.size() % size == 0);
        for (usz offset = 0; offset < bytes.size(); offset += size)
            std::reverse(bytes.begin() + offset, bytes.begin() + offset + size);
    }
    void write_pitched(u32 address, const std::vector<std::byte>& bytes, usz row_bytes, usz pitch, usz rows)
    {
        ensure(row_bytes && pitch >= row_bytes && bytes.size() == row_bytes * rows);
        ensure(rows && pitch <= UINT32_MAX / rows && address <= UINT32_MAX - pitch * rows);
        ensure(vm::check_addr(address, vm::page_writable, ::narrow<u32>(pitch * rows)));
        auto* destination = vm::get_super_ptr<std::byte>(address);
        for (usz row = 0; row < rows; ++row)
            std::memcpy(destination + row * pitch, bytes.data() + row * row_bytes, row_bytes);
    }
}

std::vector<std::byte> rsx::download_color_surface(ID3D12Resource* surface, surface_color_format format,
    ID3D12Device* device, ID3D12CommandQueue* queue, d3d12_data_heap&, resource_storage& storage, D3D12_RESOURCE_STATES state, d3d12::sample_pattern pattern)
{
    auto result = download(surface, device, queue, storage, state, 0, pattern);
    const auto pixel_size = rsx::get_format_block_size_in_bytes(format);
    const usz component_size = format == surface_color_format::w16z16y16x16 ? 2
        : format == surface_color_format::w32z32y32x32 ? 4 : pixel_size;
    reverse_components(result.bytes, component_size);
    return std::move(result.bytes);
}

std::array<std::vector<std::byte>, 2> rsx::download_depth_surface(ID3D12Resource* surface, surface_depth_format2 format,
    ID3D12Device* device, ID3D12CommandQueue* queue, d3d12_data_heap&, resource_storage& storage, D3D12_RESOURCE_STATES state, d3d12::sample_pattern pattern)
{
    auto depth = download(surface, device, queue, storage, state, 0, pattern);
    std::array<std::vector<std::byte>, 2> result;
    if (format == surface_depth_format2::z16_float)
    {
        ensure(depth.bytes.size() % 4 == 0);
        result[0].resize(depth.bytes.size() / 2);
        for (usz i = 0; i < depth.bytes.size() / 4; ++i)
        {
            u32 bits;
            std::memcpy(&bits, depth.bytes.data() + i * 4, 4);
            const auto value = d3d12::pack_depth_e4m12(bits);
            result[0][i * 2] = std::byte(value >> 8);
            result[0][i * 2 + 1] = std::byte(value & 255);
        }
        return result;
    }
    if (format == surface_depth_format2::z16_uint)
    {
        if (surface->GetDesc().SampleDesc.Count > 1)
        {
            result[0].resize(depth.bytes.size() / 2);
            for (usz i = 0; i < depth.bytes.size() / 4; ++i)
            {
                float sample;
                std::memcpy(&sample, depth.bytes.data() + i * 4, 4);
                const u16 value = static_cast<u16>(std::lround(std::clamp(sample, 0.f, 1.f) * 65535.f));
                result[0][i * 2] = std::byte(value >> 8);
                result[0][i * 2 + 1] = std::byte(value & 255);
            }
            return result;
        }
        result[0] = std::move(depth.bytes);
        reverse_components(result[0], 2);
        return result;
    }
    auto stencil = download(surface, device, queue, storage, state, 1, pattern);
    const usz pixels = static_cast<usz>(depth.width) * depth.height;
    ensure(depth.bytes.size() == pixels * sizeof(u32));
    ensure(pixels && stencil.bytes.size() % pixels == 0);
    const usz stride = stencil.bytes.size() / pixels;
    result[0].resize(pixels * 4);
    result[1].resize(pixels);
    for (usz i = 0; i < pixels; ++i)
    {
        u32 bits;
        std::memcpy(&bits, depth.bytes.data() + i * 4, 4);
        u32 z = format == surface_depth_format2::z24s8_float ? (bits >> 7) : (bits & 0x00ffffff);
        if (format == surface_depth_format2::z24s8_uint && surface->GetDesc().SampleDesc.Count > 1)
        {
            float sample;
            std::memcpy(&sample, &bits, 4);
            z = static_cast<u32>(std::llround(std::clamp(double(sample), 0., 1.) * 16777215.));
        }
        result[0][i * 4] = std::byte((z >> 16) & 255);
        result[0][i * 4 + 1] = std::byte((z >> 8) & 255);
        result[0][i * 4 + 2] = std::byte(z & 255);
        result[0][i * 4 + 3] = std::byte{0};
        const usz component = stride == 1 ? 0 : stride == 4 ? 3 : 4;
        ensure(component < stride);
        result[1][i] = stencil.bytes[i * stride + component];
    }
    return result;
}

void D3D12GSRender::copy_render_target_to_dma_location()
{
    if (g_cfg.video.write_color_buffers)
    {
        auto colors = copy_render_targets_to_memory();
        for (usz i = 0; i < colors.size(); ++i)
        {
            auto [address, surface] = m_rtts.m_bound_render_targets[i];
            if (!surface || colors[i].empty()) continue;
            const auto desc = surface->GetDesc();
            const UINT rows = desc.Height * (desc.SampleDesc.Count > 1 ? desc.SampleDesc.Count / 2 : 1);
            invalidate_address(address);
            write_pitched(address, colors[i], colors[i].size() / rows,
                rsx::method_registers.surface_pitch(::narrow<u32>(i)), rows);
        }
    }
    if (g_cfg.video.write_depth_buffer && m_rtts.m_bound_depth_stencil.second)
    {
        auto data = copy_depth_stencil_buffer_to_memory();
        auto [address, surface] = m_rtts.m_bound_depth_stencil;
        const auto desc = surface->GetDesc();
        const UINT rows = desc.Height * (desc.SampleDesc.Count > 1 ? desc.SampleDesc.Count / 2 : 1);
        if (!data[1].empty())
            for (usz i = 0; i < data[1].size(); ++i) data[0][i * 4 + 3] = data[1][i];
        invalidate_address(address);
        write_pitched(address, data[0], data[0].size() / rows,
            rsx::method_registers.surface_z_pitch(), rows);
    }
}


std::array<std::vector<std::byte>, 4> D3D12GSRender::copy_render_targets_to_memory()
{
	int clip_w = rsx::method_registers.surface_clip_width();
	int clip_h = rsx::method_registers.surface_clip_height();
	return m_rtts.get_render_targets_data(rsx::method_registers.surface_color(), clip_w, clip_h, m_device.Get(), m_command_queue.Get(), m_readback_resources, get_current_resource_storage(), D3D12_RESOURCE_STATE_RENDER_TARGET, m_active_sample_pattern);
}

std::array<std::vector<std::byte>, 2> D3D12GSRender::copy_depth_stencil_buffer_to_memory()
{
	int clip_w = rsx::method_registers.surface_clip_width();
	int clip_h = rsx::method_registers.surface_clip_height();
	return m_rtts.get_depth_stencil_data(rsx::method_registers.surface_depth_fmt(), clip_w, clip_h, m_device.Get(), m_command_queue.Get(), m_readback_resources, get_current_resource_storage(), D3D12_RESOURCE_STATE_DEPTH_WRITE, m_active_sample_pattern);
}
#endif
