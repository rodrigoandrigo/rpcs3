#ifdef _MSC_VER
#include "stdafx.h"
#include "stdafx_d3d12.h"
#include "D3D12GSRender.h"
#include "d3dx12.h"
#include "../Common/TextureUtils.h"
// For clarity this code deals with texture but belongs to D3D12GSRender class
#include "D3D12Formats.h"
#include "D3D12MSAA.h"
#include "../rsx_methods.h"

bool is_dxtc_format(u32 texture_format)
{
	switch (texture_format)
	{
	case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
	case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
	case CELL_GCM_TEXTURE_COMPRESSED_DXT45:
		return true;
	default:
		return false;
	}
}

namespace
{
D3D12_COMPARISON_FUNC get_sampler_compare_func[] =
{
	D3D12_COMPARISON_FUNC_NEVER,
	D3D12_COMPARISON_FUNC_LESS,
	D3D12_COMPARISON_FUNC_EQUAL,
	D3D12_COMPARISON_FUNC_LESS_EQUAL,
	D3D12_COMPARISON_FUNC_GREATER,
	D3D12_COMPARISON_FUNC_NOT_EQUAL,
	D3D12_COMPARISON_FUNC_GREATER_EQUAL,
	D3D12_COMPARISON_FUNC_ALWAYS
};

template <typename Texture>
D3D12_SAMPLER_DESC get_sampler_desc(const Texture &texture)
{
	D3D12_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = get_texture_filter(texture.min_filter(), texture.mag_filter());
	samplerDesc.AddressU = get_texture_wrap_mode(texture.wrap_s());
	samplerDesc.AddressV = get_texture_wrap_mode(texture.wrap_t());
	samplerDesc.AddressW = get_texture_wrap_mode(texture.wrap_r());
	if constexpr (std::is_same_v<Texture, rsx::fragment_texture>)
	{
		samplerDesc.ComparisonFunc = get_sampler_compare_func[static_cast<u8>(texture.zfunc())];
		samplerDesc.MaxAnisotropy = get_texture_max_aniso(texture.max_aniso());
	}
	else
	{
		samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		samplerDesc.MaxAnisotropy = 1;
	}
	samplerDesc.MipLODBias = texture.bias();
	const auto border = texture.remapped_border_color();
	samplerDesc.BorderColor[0] = border.r;
	samplerDesc.BorderColor[1] = border.g;
	samplerDesc.BorderColor[2] = border.b;
	samplerDesc.BorderColor[3] = border.a;
	samplerDesc.MinLOD = texture.min_lod();
	samplerDesc.MaxLOD = texture.max_lod();
	return samplerDesc;
}

namespace
{
	template <typename Texture>
CD3DX12_RESOURCE_DESC get_texture_description(const Texture &texture)
	{
		const u8 format = texture.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
		DXGI_FORMAT dxgi_format = get_texture_format(format);
		u16 width = texture.width();
		u16 height = texture.height();
		u16 depth = texture.depth();
		u16 miplevels = texture.get_exact_mipmap_count();

		// DXTC uses 4x4 block texture and align to multiple of 4.
		if (is_dxtc_format(format))
		{
			width = utils::align(width, 4);
			height = utils::align(height, 4);
		}

		switch (texture.get_extended_texture_dimension())
		{
		case rsx::texture_dimension_extended::texture_dimension_1d:
			return CD3DX12_RESOURCE_DESC::Tex1D(dxgi_format, width, 1, miplevels);
		case rsx::texture_dimension_extended::texture_dimension_2d:
			return CD3DX12_RESOURCE_DESC::Tex2D(dxgi_format, width, height, 1, miplevels);
		case rsx::texture_dimension_extended::texture_dimension_cubemap:
			return CD3DX12_RESOURCE_DESC::Tex2D(dxgi_format, width, height, 6, miplevels);
		case rsx::texture_dimension_extended::texture_dimension_3d:
			return CD3DX12_RESOURCE_DESC::Tex3D(dxgi_format, width, height, depth, miplevels);
		}
		fmt::throw_exception("Unknown texture dimension");
	}
}

namespace {
	/**
	 * Allocate buffer in texture_buffer_heap big enough and upload data into existing_texture which should be in COPY_DEST state
	 */
	template <typename Texture>
void update_existing_texture(
		const Texture &texture,
		ID3D12GraphicsCommandList *command_list,
		d3d12_data_heap &texture_buffer_heap,
		ID3D12Resource *existing_texture)
	{
		size_t w = texture.width(), h = texture.height();

		const u8 format = texture.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
		DXGI_FORMAT dxgi_format = get_texture_format(format);

		size_t buffer_size = get_placed_texture_storage_size(texture, 256);
		size_t heap_offset = texture_buffer_heap.alloc<D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT>(buffer_size);
		size_t mip_level = 0;

		void *mapped_buffer_ptr = texture_buffer_heap.map<void>(CD3DX12_RANGE(heap_offset, heap_offset + buffer_size));
		std::span<std::byte> mapped_buffer{ (std::byte*)mapped_buffer_ptr, buffer_size };
		const auto input_layouts = rsx::get_subresources_layout(texture);
		u8 block_size_in_bytes = rsx::get_format_block_size_in_bytes(format);
		u8 block_size_in_texel = rsx::get_format_block_size_in_texel(format);
		rsx::texture_uploader_capabilities capabilities{};
		capabilities.supports_dxt = true;
		capabilities.alignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
		bool is_swizzled = !(texture.format() & CELL_GCM_TEXTURE_LN);
		size_t offset_in_buffer = 0;
		for (const rsx::subresource_layout &layout : input_layouts)
		{
			rsx::io_buffer destination(mapped_buffer.subspan(offset_in_buffer));
			const auto upload = rsx::upload_texture_subresource(destination, layout, format, is_swizzled, capabilities);
			ensure(!upload.require_upload && !upload.require_swap && !upload.require_deswizzle, "D3D12 CPU texture uploader left unfinished GPU conversion work");
			UINT row_pitch = utils::align(static_cast<u32>(layout.width_in_block) * block_size_in_bytes, 256u);
			command_list->CopyTextureRegion(d3d12::address_of(CD3DX12_TEXTURE_COPY_LOCATION(existing_texture, (UINT)mip_level)), 0, 0, 0,
				d3d12::address_of(CD3DX12_TEXTURE_COPY_LOCATION(texture_buffer_heap.get_heap(),
				{ heap_offset + offset_in_buffer,
				{
					dxgi_format,
					(UINT)layout.width_in_block * block_size_in_texel,
					(UINT)layout.height_in_block * block_size_in_texel,
					(UINT)layout.depth,
					row_pitch
				}
				})), nullptr);

			offset_in_buffer += row_pitch * layout.height_in_block * layout.depth;
			offset_in_buffer = utils::align(offset_in_buffer, 512);
			mip_level++;
		}
		texture_buffer_heap.unmap(CD3DX12_RANGE(heap_offset, heap_offset + buffer_size));

		command_list->ResourceBarrier(1, d3d12::address_of(CD3DX12_RESOURCE_BARRIER::Transition(existing_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ)));
	}
}


/**
 * Create a texture residing in default heap and generate uploads commands in commandList,
 * using a temporary texture buffer.
 */
template <typename Texture>
ComPtr<ID3D12Resource> create_texture(
	const Texture &texture,
	ID3D12Device *device)
{
	ComPtr<ID3D12Resource> result;
	CHECK_HRESULT(device->CreateCommittedResource(
		d3d12::address_of(CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT)),
		D3D12_HEAP_FLAG_NONE,
		d3d12::address_of(get_texture_description(texture)),
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(result.GetAddressOf())
		));

	return result;
}


template <typename Texture>
D3D12_SHADER_RESOURCE_VIEW_DESC get_srv_descriptor_with_dimensions(const Texture &tex)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC shared_resource_view_desc = {};
	switch (tex.get_extended_texture_dimension())
	{
	case rsx::texture_dimension_extended::texture_dimension_1d:
		shared_resource_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
		shared_resource_view_desc.Texture1D.MipLevels = tex.get_exact_mipmap_count();
		return shared_resource_view_desc;
	case rsx::texture_dimension_extended::texture_dimension_2d:
		shared_resource_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		shared_resource_view_desc.Texture2D.MipLevels = tex.get_exact_mipmap_count();
		return shared_resource_view_desc;
	case rsx::texture_dimension_extended::texture_dimension_cubemap:
		shared_resource_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		shared_resource_view_desc.TextureCube.MipLevels = tex.get_exact_mipmap_count();
		return shared_resource_view_desc;
	case rsx::texture_dimension_extended::texture_dimension_3d:
		shared_resource_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		shared_resource_view_desc.Texture3D.MipLevels = tex.get_exact_mipmap_count();
		return shared_resource_view_desc;
	}
	fmt::throw_exception("Wrong texture dimension");
}
}

template <typename Texture>
void D3D12GSRender::upload_texture_bank(ID3D12GraphicsCommandList *command_list, size_t texture_count,
	Texture* textures, ID3D12DescriptorHeap* texture_descriptors, ID3D12DescriptorHeap* sampler_descriptors)
{
	for (u32 i = 0; i < texture_count; ++i)
	{
		// CPU invalidation is independent of changes to texture registers.
		// Revisit cache entries and attachment feedback on every draw.
		if constexpr (std::is_same_v<Texture, rsx::vertex_texture>) m_vertex_textures_dirty[i] = false;
		else m_textures_dirty[i] = false;

		if (!textures[i].enabled())
		{
			// Now fill remaining texture slots with dummy texture/sampler

			D3D12_SHADER_RESOURCE_VIEW_DESC shader_resource_view_desc = {};
			const auto dimension = [&]()
			{
				if constexpr (std::is_same_v<Texture, rsx::vertex_texture>) return current_vertex_program.get_texture_dimension(i);
				else return current_fragment_program.get_texture_dimension(i);
			}();
			switch (dimension)
			{
			case rsx::texture_dimension_extended::texture_dimension_1d:
				shader_resource_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
				shader_resource_view_desc.Texture1D.MipLevels = 1;
				break;
			case rsx::texture_dimension_extended::texture_dimension_2d:
				shader_resource_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				shader_resource_view_desc.Texture2D.MipLevels = 1;
				break;
			case rsx::texture_dimension_extended::texture_dimension_cubemap:
				shader_resource_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
				shader_resource_view_desc.TextureCube.MipLevels = 1;
				break;
			case rsx::texture_dimension_extended::texture_dimension_3d:
				shader_resource_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
				shader_resource_view_desc.Texture3D.MipLevels = 1;
				break;
			}
			shader_resource_view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			shader_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
				D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
				D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
				D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);

			m_device->CreateShaderResourceView(nullptr, &shader_resource_view_desc,
				CD3DX12_CPU_DESCRIPTOR_HANDLE(texture_descriptors->GetCPUDescriptorHandleForHeapStart())
				.Offset((UINT)i, m_descriptor_stride_srv_cbv_uav)
				);

			D3D12_SAMPLER_DESC sampler_desc = {};
			sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
			sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			sampler_desc.MaxAnisotropy = 1;

			m_device->CreateSampler(&sampler_desc,
				CD3DX12_CPU_DESCRIPTOR_HANDLE(sampler_descriptors->GetCPUDescriptorHandleForHeapStart())
				.Offset((UINT)i, m_descriptor_stride_samplers));

			continue;
		}
		size_t w = textures[i].width(), h = textures[i].height();
		
		if (!w || !h)
		{
			fmt::throw_exception("D3D12 texture upload requested with zero dimensions");
		}

		const u32 texaddr = rsx::get_address(textures[i].offset(), textures[i].location());

		const u8 format = textures[i].format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
		bool is_swizzled = !(textures[i].format() & CELL_GCM_TEXTURE_LN);
		const u64 texture_layout = (u64(textures[i].pitch()) << 32) |
			(u64(static_cast<u8>(textures[i].get_extended_texture_dimension())) << 1) | !is_swizzled;

		ID3D12Resource *vram_texture;
		auto cached_texture = m_texture_cache.find_data_if_available(texaddr);
		bool is_render_target = false, is_depth_stencil_texture = false;

		if (vram_texture = m_rtts.get_texture_from_render_target_if_applicable(texaddr))
		{
			is_render_target = true;
		}
		else if (vram_texture = m_rtts.get_texture_from_depth_stencil_if_applicable(texaddr))
		{
			is_depth_stencil_texture = true;
		}
		else if (cached_texture && (cached_texture->first == texture_entry(format, w, h, textures[i].depth(), textures[i].get_exact_mipmap_count(), texture_layout)))
		{
			if (cached_texture->first.m_is_dirty)
			{
				command_list->ResourceBarrier(1, d3d12::address_of(CD3DX12_RESOURCE_BARRIER::Transition(cached_texture->second.Get(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST)));
				m_texture_cache.protect_data(texaddr, texaddr, get_texture_size(textures[i]));
				update_existing_texture(textures[i], command_list, m_buffer_data, cached_texture->second.Get());
			}
			vram_texture = cached_texture->second.Get();
		}
		else
		{
			if (cached_texture)
				get_current_resource_storage().dirty_textures.push_back(m_texture_cache.remove_from_cache(texaddr));
			ComPtr<ID3D12Resource> tex = create_texture(textures[i], m_device.Get());
			std::wstring name = L"texture_@" + std::to_wstring(texaddr);
			tex->SetName(name.c_str());
			vram_texture = tex.Get();
			m_texture_cache.store_and_protect_data(texaddr, texaddr, get_texture_size(textures[i]), format, w, h, textures[i].depth(), textures[i].get_exact_mipmap_count(), tex, texture_layout);
			// Protect before reading guest bytes, not after the upload. A CPU
			// write racing the copy must leave this entry dirty for the next draw.
			update_existing_texture(textures[i], command_list, m_buffer_data, tex.Get());
		}

		D3D12_SHADER_RESOURCE_VIEW_DESC shared_resource_view_desc = get_srv_descriptor_with_dimensions(textures[i]);
		if (is_render_target || is_depth_stencil_texture)
		{
			bool bound = is_depth_stencil_texture && m_rtts.m_bound_depth_stencil.second == vram_texture;
			for (const auto& target : m_rtts.m_bound_render_targets)
				bound |= target.second && target.second == vram_texture;
			if (vram_texture->GetDesc().SampleDesc.Count > 1)
			{
				const auto source_state = bound
					? (is_depth_stencil_texture ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET)
					: D3D12_RESOURCE_STATE_GENERIC_READ;
				auto samples = d3d12::expand_samples(m_device.Get(), command_list, vram_texture, source_state, is_depth_stencil_texture, 0, &m_sample_pipeline_cache, m_active_sample_pattern);
				vram_texture = samples->image.Get();
				get_current_resource_storage().temporary_passes.push_back(std::move(samples));
			}
			else if (bound)
			{
				// Sampling a currently writable attachment needs a snapshot, not
				// simultaneous SRV/RTV binding of the same subresource.
				ComPtr<ID3D12Resource> snapshot;
				auto desc = vram_texture->GetDesc();
				desc.Flags = D3D12_RESOURCE_FLAG_NONE;
				CHECK_HRESULT(m_device->CreateCommittedResource(
					d3d12::address_of(CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT)), D3D12_HEAP_FLAG_NONE,
					&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(snapshot.GetAddressOf())));
				const auto write_state = is_depth_stencil_texture ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
				command_list->ResourceBarrier(1, d3d12::address_of(CD3DX12_RESOURCE_BARRIER::Transition(vram_texture, write_state, D3D12_RESOURCE_STATE_COPY_SOURCE)));
				command_list->CopyResource(snapshot.Get(), vram_texture);
				command_list->ResourceBarrier(1, d3d12::address_of(CD3DX12_RESOURCE_BARRIER::Transition(vram_texture, D3D12_RESOURCE_STATE_COPY_SOURCE, write_state)));
				command_list->ResourceBarrier(1, d3d12::address_of(CD3DX12_RESOURCE_BARRIER::Transition(snapshot.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ)));
				vram_texture = snapshot.Get();
				get_current_resource_storage().dirty_textures.push_back(std::move(snapshot));
			}
		}
		shared_resource_view_desc.Format = get_texture_format(format);

		bool requires_remap = false;
		std::array<INT, 4> channel_mapping = {};

		switch (format)
		{
		default:
			rsx_log.error("Unimplemented mapping for texture format: 0x%x", format);
			break;
		
		case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
		case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
		case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
		case CELL_GCM_TEXTURE_COMPRESSED_DXT45:
		case CELL_GCM_TEXTURE_DEPTH24_D8:
		case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
		case CELL_GCM_TEXTURE_DEPTH16:
		case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
		case CELL_GCM_TEXTURE_X32_FLOAT:
		case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
		case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT:
		case CELL_GCM_TEXTURE_R5G5B5A1:
		case CELL_GCM_TEXTURE_D1R5G5B5:
		case CELL_GCM_TEXTURE_A1R5G5B5:
		case CELL_GCM_TEXTURE_A4R4G4B4:
		case CELL_GCM_TEXTURE_R5G6B5:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			break;

		case CELL_GCM_TEXTURE_B8:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0);
			break;

		case CELL_GCM_TEXTURE_G8B8:
		{
			u8 remap_a = textures[i].remap() & 0x3;
			u8 remap_r = (textures[i].remap() >> 2) & 0x3;
			u8 remap_g = (textures[i].remap() >> 4) & 0x3;
			u8 remap_b = (textures[i].remap() >> 6) & 0x3;

			if (is_render_target)
			{
				// ARGB format
				// Data comes from RTT, stored as RGBA already
				const int RemapValue[4] =
				{
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2
				};

				channel_mapping = { RemapValue[remap_r], RemapValue[remap_g], RemapValue[remap_b], RemapValue[remap_a] };
				requires_remap = true;
			}
			else
			{
				// ARGB format
				// Data comes from RSX mem, stored as ARGB already
				const int RemapValue[4] =
				{
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1
				};

				channel_mapping = { RemapValue[remap_r], RemapValue[remap_g], RemapValue[remap_b], RemapValue[remap_a] };
				requires_remap = true;
			}

			break;
		}

		case CELL_GCM_TEXTURE_R6G5B5: // TODO: Remap it to another format here, so it's not glitched out
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
				D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
			break;

		case CELL_GCM_TEXTURE_X16:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0);
			break;

		case CELL_GCM_TEXTURE_Y16_X16:
		case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0);
			break;

		case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1);
			break;

		case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
		case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
			break;
				
		case CELL_GCM_TEXTURE_D8R8G8B8:	
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3,
				D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
			break;
			
		case CELL_GCM_TEXTURE_A8R8G8B8:
		{
			u8 remap_a = textures[i].remap() & 0x3;
			u8 remap_r = (textures[i].remap() >> 2) & 0x3;
			u8 remap_g = (textures[i].remap() >> 4) & 0x3;
			u8 remap_b = (textures[i].remap() >> 6) & 0x3;

			if (is_render_target)
			{
				// ARGB format
				// Data comes from RTT, stored as RGBA already
				const int RemapValue[4] =
				{
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2
				};

				channel_mapping = { RemapValue[remap_r], RemapValue[remap_g], RemapValue[remap_b], RemapValue[remap_a] };
				requires_remap = true;
			}
			else if (is_depth_stencil_texture)
			{
				shared_resource_view_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
				shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0);
			}
			else
			{
				// ARGB format
				// Data comes from RSX mem, stored as ARGB already
				const int RemapValue[4] =
				{
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3
				};

				channel_mapping = { RemapValue[remap_r], RemapValue[remap_g], RemapValue[remap_b], RemapValue[remap_a] };
				requires_remap = true;
			}

			break;
		}
		}

		if (requires_remap)
		{
			auto decoded_remap = textures[i].decoded_remap();
			u8 remapped_inputs[] = { decoded_remap.control_map[1], decoded_remap.control_map[2], decoded_remap.control_map[3], decoded_remap.control_map[0] };

			for (u8 channel = 0; channel < 4; channel++)
			{
				switch (remapped_inputs[channel])
				{
				default:
				case CELL_GCM_TEXTURE_REMAP_REMAP:
					break;
				case CELL_GCM_TEXTURE_REMAP_ONE:
					channel_mapping[channel] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
					break;
				case CELL_GCM_TEXTURE_REMAP_ZERO:
					channel_mapping[channel] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
				}
			}

			shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				channel_mapping[0],
				channel_mapping[1],
				channel_mapping[2],
				channel_mapping[3]);
		}

		if (is_render_target || is_depth_stencil_texture)
		{
			ensure(shared_resource_view_desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D,
				"D3D12 surface aliases require a 2D sampler");
			shared_resource_view_desc.Texture2D.MipLevels = 1;
			shared_resource_view_desc.Format = vram_texture->GetDesc().Format;
			if (is_depth_stencil_texture)
			{
				switch (shared_resource_view_desc.Format)
				{
				case DXGI_FORMAT_R16_TYPELESS: shared_resource_view_desc.Format = DXGI_FORMAT_R16_UNORM; break;
				case DXGI_FORMAT_R24G8_TYPELESS: shared_resource_view_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
				case DXGI_FORMAT_R32_TYPELESS: shared_resource_view_desc.Format = DXGI_FORMAT_R32_FLOAT; break;
				case DXGI_FORMAT_R32_FLOAT: break;
				case DXGI_FORMAT_R32G8X24_TYPELESS: shared_resource_view_desc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
				default: fmt::throw_exception("Unsupported D3D12 depth surface alias");
				}
			}
		}
		m_device->CreateShaderResourceView(vram_texture, &shared_resource_view_desc,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(texture_descriptors->GetCPUDescriptorHandleForHeapStart())
			.Offset((UINT)i, m_descriptor_stride_srv_cbv_uav)
			);

		m_device->CreateSampler(d3d12::address_of(get_sampler_desc(textures[i])),
			CD3DX12_CPU_DESCRIPTOR_HANDLE(sampler_descriptors->GetCPUDescriptorHandleForHeapStart())
			.Offset((UINT)i, m_descriptor_stride_samplers));
	}
}
void D3D12GSRender::upload_textures(ID3D12GraphicsCommandList* command_list, size_t texture_count)
{
	upload_texture_bank(command_list, texture_count, rsx::method_registers.fragment_textures.data(),
		m_current_texture_descriptors.Get(), m_current_sampler_descriptors.Get());
}

void D3D12GSRender::upload_vertex_textures(ID3D12GraphicsCommandList* command_list)
{
	upload_texture_bank(command_list, 4, rsx::method_registers.vertex_textures.data(),
		m_current_vertex_texture_descriptors.Get(), m_current_vertex_sampler_descriptors.Get());
}
#endif
