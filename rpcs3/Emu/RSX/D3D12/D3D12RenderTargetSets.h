#pragma once

#define INCOMPLETE_SURFACE_CACHE_IMPL

#include <utility>
#include <span>
#include <d3d12.h>
#include "d3dx12.h"

#include "D3D12Formats.h"
#include "D3D12MemoryHelpers.h"
#include "D3D12SurfaceRanges.h"
#include "D3D12SamplePositions.h"

namespace rsx
{
inline UINT d3d12_sample_count(surface_antialiasing mode)
{
	switch (mode)
	{
	case surface_antialiasing::center_1_sample: return 1;
	case surface_antialiasing::diagonal_centered_2_samples: return 2;
	case surface_antialiasing::square_centered_4_samples:
	case surface_antialiasing::square_rotated_4_samples: return 4;
	}
	fmt::throw_exception("Invalid D3D12 surface antialiasing mode");
}
std::vector<std::byte> download_color_surface(ID3D12Resource*, surface_color_format,
    ID3D12Device*, ID3D12CommandQueue*, d3d12_data_heap&, resource_storage&, D3D12_RESOURCE_STATES = D3D12_RESOURCE_STATE_RENDER_TARGET,
    d3d12::sample_pattern = d3d12::sample_pattern::center);
std::array<std::vector<std::byte>, 2> download_depth_surface(ID3D12Resource*, surface_depth_format2,
    ID3D12Device*, ID3D12CommandQueue*, d3d12_data_heap&, resource_storage&, D3D12_RESOURCE_STATES = D3D12_RESOURCE_STATE_DEPTH_WRITE,
    d3d12::sample_pattern = d3d12::sample_pattern::center);
namespace utility
{
	std::vector<u8> get_d3d12_rtt_indexes(surface_target color_target);
	size_t get_aligned_pitch(surface_color_format format, u32 width);
	size_t get_packed_pitch(surface_color_format format, u32 width);
}

template<typename Traits>
struct surface_store_deprecated
{
	template<typename T, typename U>
	void copy_pitched_src_to_dst(std::span<T> dest, std::span<const U> src, size_t src_pitch_in_bytes, size_t width, size_t height)
	{
		for (unsigned row = 0; row < height; row++)
		{
			for (unsigned col = 0; col < width; col++)
				dest[col] = src[col];
			src = src.subspan(src_pitch_in_bytes / sizeof(U));
			dest = dest.subspan(width);
		}
	}

public:
	using surface_storage_type = typename Traits::surface_storage_type;
	using surface_type = typename Traits::surface_type;
	using command_list_type = typename Traits::command_list_type;
	using download_buffer_object = typename Traits::download_buffer_object;

protected:
	std::unordered_map<u32, surface_storage_type> m_render_targets_storage = {};
	std::unordered_map<u32, surface_storage_type> m_depth_stencil_storage = {};
	std::unordered_map<u32, u64> m_color_memory_sizes;
	std::unordered_map<u32, u64> m_depth_memory_sizes;

public:
	std::pair<u8, u8> m_bound_render_targets_config = {};
	std::array<std::pair<u32, surface_type>, 4> m_bound_render_targets = {};
	std::pair<u32, surface_type> m_bound_depth_stencil = {};

	std::list<surface_storage_type> invalidated_resources;
	bool invalidate_memory_range(u32 address, u32 size)
	{
		bool changed = false;
		auto invalidate = [&](auto& images, auto& sizes)
		{
			changed |= d3d12::invalidate_surface_memory(images, sizes, invalidated_resources, address, size,
				[&](const auto& image)
			{
					auto* resource = Traits::get(image);
					for (auto& bound : m_bound_render_targets)
						if (bound.second == resource) bound = {};
					if (m_bound_depth_stencil.second == resource) m_bound_depth_stencil = {};
			});
		};
		if (size)
		{
			invalidate(m_render_targets_storage, m_color_memory_sizes);
			invalidate(m_depth_stencil_storage, m_depth_memory_sizes);
		}
		return changed;
	}

	surface_store_deprecated() = default;
	~surface_store_deprecated() = default;
	surface_store_deprecated(const surface_store_deprecated&) = delete;

protected:
	/**
	* If render target already exists at address, issue state change operation on cmdList.
	* Otherwise create one with width, height, clearColor info.
	* returns the corresponding render target resource.
	*/
	template <typename ...Args>
	surface_type bind_address_as_render_targets(
		command_list_type command_list,
		u32 address,
		surface_color_format color_format,
		surface_antialiasing antialias,
		size_t width, size_t height, size_t pitch,
		Args&&... extra_params)
	{
		// Check if render target already exists
		m_color_memory_sizes[address] = u64(pitch) * height * (d3d12_sample_count(antialias) > 1 ? d3d12_sample_count(antialias) / 2 : 1);
		auto It = m_render_targets_storage.find(address);
		if (It != m_render_targets_storage.end())
		{
			surface_storage_type &rtt = It->second;
			if (Traits::rtt_has_format_width_height(rtt, color_format, width, height) && rtt->GetDesc().SampleDesc.Count == d3d12_sample_count(antialias))
			{
				Traits::prepare_rtt_for_drawing(command_list, Traits::get(rtt));
				return Traits::get(rtt);
			}

			invalidated_resources.push_back(std::move(rtt));
			m_render_targets_storage.erase(It);
		}

		m_render_targets_storage[address] = Traits::create_new_surface(address, color_format, width, height, pitch, antialias, std::forward<Args>(extra_params)...);
		return Traits::get(m_render_targets_storage[address]);
	}

	template <typename ...Args>
	surface_type bind_address_as_depth_stencil(
		command_list_type command_list,
		u32 address,
		surface_depth_format2 depth_format,
		surface_antialiasing antialias,
		size_t width, size_t height, size_t pitch,
		Args&&... extra_params)
	{
		auto It = m_depth_stencil_storage.find(address);
		m_depth_memory_sizes[address] = u64(pitch) * height * (d3d12_sample_count(antialias) > 1 ? d3d12_sample_count(antialias) / 2 : 1);
		if (It != m_depth_stencil_storage.end())
		{
			surface_storage_type &ds = It->second;
			if (Traits::ds_has_format_width_height(ds, depth_format, width, height) && ds->GetDesc().SampleDesc.Count == d3d12_sample_count(antialias))
			{
				Traits::prepare_ds_for_drawing(command_list, Traits::get(ds));
				return Traits::get(ds);
			}

			invalidated_resources.push_back(std::move(ds));
			m_depth_stencil_storage.erase(It);
		}

		m_depth_stencil_storage[address] = Traits::create_new_surface(address, depth_format, width, height, pitch, antialias, std::forward<Args>(extra_params)...);
		return Traits::get(m_depth_stencil_storage[address]);
	}
public:
	/**
		* Update bound color and depth surface.
		* Must be called everytime surface format, clip, or addresses changes.
		*/
	template <typename ...Args>
	void prepare_render_target(
		command_list_type command_list,
		surface_color_format color_format, surface_depth_format2 depth_format,
		u32 clip_horizontal_reg, u32 clip_vertical_reg,
		surface_target set_surface_target,
		surface_antialiasing antialias,
		const std::array<u32, 4> &surface_addresses, u32 address_z,
		const std::array<u32, 4> &surface_pitch, u32 zeta_pitch,
		Args&&... extra_params)
	{
		u32 clip_width = clip_horizontal_reg;
		u32 clip_height = clip_vertical_reg;

		// Make previous RTTs sampleable
		for (auto& rtt : m_bound_render_targets)
		{
			if (rtt.second)
				Traits::prepare_rtt_for_sampling(command_list, rtt.second);
			rtt = std::make_pair(0, nullptr);
		}

		const auto rtt_indices = utility::get_d3d12_rtt_indexes(set_surface_target);
		if (!rtt_indices.empty())
		{
			m_bound_render_targets_config = { rtt_indices.front(), 0 };

			// Create/Reuse requested rtts
			for (u8 surface_index : rtt_indices)
			{
				if (surface_addresses[surface_index] == 0)
					continue;

				m_bound_render_targets[surface_index] = std::make_pair(surface_addresses[surface_index],
					bind_address_as_render_targets(command_list, surface_addresses[surface_index], color_format, antialias,
						clip_width, clip_height, surface_pitch[surface_index], std::forward<Args>(extra_params)...));

				m_bound_render_targets_config.second++;
			}
		}
		else
		{
			m_bound_render_targets_config = { 0, 0 };
		}

		// Same for depth buffer
		if (std::get<1>(m_bound_depth_stencil) != nullptr)
			Traits::prepare_ds_for_sampling(command_list, std::get<1>(m_bound_depth_stencil));

		m_bound_depth_stencil = std::make_pair(0, nullptr);

		if (!address_z)
			return;

		m_bound_depth_stencil = std::make_pair(address_z,
			bind_address_as_depth_stencil(command_list, address_z, depth_format, antialias,
				clip_width, clip_height, zeta_pitch, std::forward<Args>(extra_params)...));
	}

	/**
		* Search for given address in stored color surface
		* Return an empty surface_type otherwise.
		*/
	surface_type get_texture_from_render_target_if_applicable(u32 address)
	{
		auto It = m_render_targets_storage.find(address);
		if (It != m_render_targets_storage.end())
			return Traits::get(It->second);
		return surface_type();
	}

	/**
	* Search for given address in stored depth stencil surface
	* Return an empty surface_type otherwise.
	*/
	surface_type get_texture_from_depth_stencil_if_applicable(u32 address)
	{
		auto It = m_depth_stencil_storage.find(address);
		if (It != m_depth_stencil_storage.end())
			return Traits::get(It->second);
		return surface_type();
	}

	/**
		* Get bound color surface raw data.
		*/
	template <typename... Args>
	std::array<std::vector<std::byte>, 4> get_render_targets_data(
		surface_color_format color_format, size_t width, size_t height,
		Args&& ...args
	)
	{
		std::array<std::vector<std::byte>, 4> result;
		for (usz i = 0; i < result.size(); ++i)
			if (m_bound_render_targets[i].second)
				result[i] = download_color_surface(m_bound_render_targets[i].second, color_format, args...);
		return result;
	}

	/**
		* Get bound color surface raw data.
		*/
	template <typename... Args>
	std::array<std::vector<std::byte>, 2> get_depth_stencil_data(
		surface_depth_format2 depth_format, size_t width, size_t height,
		Args&& ...args
	)
	{
		if (!m_bound_depth_stencil.second) return {};
		return download_depth_surface(m_bound_depth_stencil.second, depth_format, args...);
	}
};

struct render_target_traits
{
	using surface_storage_type = ComPtr<ID3D12Resource>;
	using surface_type = ID3D12Resource*;
	using command_list_type = ID3D12GraphicsCommandList*;
	using download_buffer_object = std::tuple<size_t, size_t, size_t, ComPtr<ID3D12Fence>, HANDLE>; // heap offset, size, last_put_pos, fence, handle

	//TODO: Move this somewhere else
	bool depth_is_dirty = false;

	static
	ComPtr<ID3D12Resource> create_new_surface(
		u32 address,
		surface_color_format color_format, size_t width, size_t height, size_t /*pitch*/,
		surface_antialiasing antialias,
		ID3D12Device* device, const std::array<float, 4> &clear_color, float, u8)
	{
		DXGI_FORMAT dxgi_format = get_color_surface_format(color_format);
		ComPtr<ID3D12Resource> rtt;
		rsx_log.warning("Creating RTT");

		D3D12_CLEAR_VALUE clear_color_value = {};
		clear_color_value.Format = dxgi_format;
		clear_color_value.Color[0] = clear_color[0];
		clear_color_value.Color[1] = clear_color[1];
		clear_color_value.Color[2] = clear_color[2];
		clear_color_value.Color[3] = clear_color[3];

		const CD3DX12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE_DEFAULT);
		const UINT samples = d3d12_sample_count(antialias);
		D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{dxgi_format, samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
		CHECK_HRESULT(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality)));
		ensure(quality.NumQualityLevels, "D3D12 color format does not support the requested sample count");
		const auto resource_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgi_format, (UINT)width, (UINT)height, 1, 1, samples, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
		CHECK_HRESULT(device->CreateCommittedResource(
			&heap_properties,
			D3D12_HEAP_FLAG_NONE,
			&resource_desc,
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			&clear_color_value,
			IID_PPV_ARGS(rtt.GetAddressOf())
			));

		std::wstring name = L"rtt_@" + std::to_wstring(address);
		rtt->SetName(name.c_str());

		return rtt;
	}

	static
	void prepare_rtt_for_drawing(
		ID3D12GraphicsCommandList* command_list,
		ID3D12Resource* rtt)
	{
		const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(rtt, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
		command_list->ResourceBarrier(1, &barrier);
	}

	static
	void prepare_rtt_for_sampling(
		ID3D12GraphicsCommandList* command_list,
		ID3D12Resource* rtt)
	{
		const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(rtt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
		command_list->ResourceBarrier(1, &barrier);
	}

	static
	ComPtr<ID3D12Resource> create_new_surface(
		u32 address,
		surface_depth_format2 surfaceDepthFormat, size_t width, size_t height, size_t /*pitch*/,
		surface_antialiasing antialias,
		ID3D12Device* device, const std::array<float, 4>& , float clear_depth, u8 clear_stencil)
	{
		D3D12_CLEAR_VALUE clear_depth_value = {};
		clear_depth_value.DepthStencil.Depth = clear_depth;
		clear_depth_value.DepthStencil.Stencil = clear_stencil;

		DXGI_FORMAT dxgi_format = get_depth_stencil_typeless_surface_format(surfaceDepthFormat);
		clear_depth_value.Format = get_depth_stencil_surface_clear_format(surfaceDepthFormat);

		ComPtr<ID3D12Resource> new_depth_stencil;
		const CD3DX12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE_DEFAULT);
		const UINT samples = d3d12_sample_count(antialias);
		D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{clear_depth_value.Format, samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
		CHECK_HRESULT(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality)));
		ensure(quality.NumQualityLevels, "D3D12 depth format does not support the requested sample count");
		const auto resource_desc = CD3DX12_RESOURCE_DESC::Tex2D(dxgi_format, (UINT)width, (UINT)height, 1, 1, samples, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
		CHECK_HRESULT(device->CreateCommittedResource(
			&heap_properties,
			D3D12_HEAP_FLAG_NONE,
			&resource_desc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&clear_depth_value,
			IID_PPV_ARGS(new_depth_stencil.GetAddressOf())
			));
		std::wstring name = L"ds_@" + std::to_wstring(address);
		new_depth_stencil->SetName(name.c_str());

		return new_depth_stencil;
	}

	static
	void prepare_ds_for_drawing(
		ID3D12GraphicsCommandList* command_list,
		ID3D12Resource* ds)
	{
		// set the resource as depth write
		const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(ds, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		command_list->ResourceBarrier(1, &barrier);
	}

	static
	void prepare_ds_for_sampling(
		ID3D12GraphicsCommandList* command_list,
		ID3D12Resource* ds)
	{
		const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(ds, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ);
		command_list->ResourceBarrier(1, &barrier);
	}

	static
	bool rtt_has_format_width_height(const ComPtr<ID3D12Resource> &rtt, surface_color_format surface_color_format, size_t width, size_t height, bool=false)
	{
		DXGI_FORMAT dxgi_format = get_color_surface_format(surface_color_format);
		return rtt->GetDesc().Format == dxgi_format && rtt->GetDesc().Width == width && rtt->GetDesc().Height == height;
	}

	static
	bool ds_has_format_width_height(const ComPtr<ID3D12Resource> &rtt, surface_depth_format2 format, size_t width, size_t height, bool=false)
	{
		return rtt->GetDesc().Format == get_depth_stencil_typeless_surface_format(format)
			&& rtt->GetDesc().Width == width && rtt->GetDesc().Height == height;
	}


	static ID3D12Resource* get(const ComPtr<ID3D12Resource> &in)
	{
		return in.Get();
	}
};

struct render_targets : public rsx::surface_store_deprecated<render_target_traits>
{
	INT g_descriptor_stride_rtv;

	D3D12_CPU_DESCRIPTOR_HANDLE current_rtts_handle;
	D3D12_CPU_DESCRIPTOR_HANDLE current_ds_handle;

	void init(ID3D12Device *device);
};

}
