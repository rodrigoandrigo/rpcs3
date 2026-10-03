#ifdef _MSC_VER
#include "stdafx.h"
#include "stdafx_d3d12.h"

#include <variant>

#include "D3D12GSRender.h"
#include "D3D12ShaderConstants.h"
#include "d3dx12.h"
#include "../Common/BufferUtils.h"
#include "D3D12Formats.h"
#include "../rsx_methods.h"



void D3D12GSRender::upload_and_bind_scale_offset_matrix(size_t descriptorIndex)
{
	constexpr auto allocation_size = d3d12::shader_constants_allocation_size;
	size_t heap_offset = m_buffer_data.alloc<D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT>(allocation_size);

	// Scale offset buffer
	// Separate constant buffer
	void *mapped_buffer = m_buffer_data.map<void>(CD3DX12_RANGE(heap_offset, heap_offset + allocation_size));
	std::memset(mapped_buffer, 0, allocation_size);
	d3d12::shader_constants constants{};
	GRAPH_frontend().fill_scale_offset_data(constants.scale_offset, true);
	u32 clip_configuration = 0;
	GRAPH_frontend().fill_user_clip_data(&clip_configuration);
	d3d12::expand_clip_configuration(constants, clip_configuration);
	constants.fog_param0 = rsx::method_registers.fog_params_0();
	constants.fog_param1 = rsx::method_registers.fog_params_1();
	constants.fog_mode = static_cast<u32>(rsx::method_registers.fog_equation());
	constants.alpha_test = rsx::method_registers.alpha_test_enabled() ? 1 : 0;
	constants.alpha_ref = rsx::method_registers.alpha_ref();
	constants.alpha_func = static_cast<u32>(rsx::method_registers.alpha_func());
	const bool top_origin = rsx::method_registers.shader_window_origin() == rsx::window_origin::top;
	constants.wpos_scale = top_origin ? 1.f : -1.f;
	constants.wpos_bias = top_origin ? 0.f : static_cast<float>(rsx::method_registers.shader_window_height());
	static_assert(sizeof(constants.textures) == sizeof(current_fragment_program.texture_params));
	static_assert(sizeof(d3d12::texture_constants) == sizeof(rsx::fragment_program_texture_config::TIU_slot));
	static_assert(offsetof(d3d12::texture_constants, control) == offsetof(rsx::fragment_program_texture_config::TIU_slot, control));
	std::memcpy(constants.textures, &current_fragment_program.texture_params, sizeof(constants.textures));
	std::memcpy(constants.input_attributes, m_vertex_layout_state.data(), sizeof(constants.input_attributes));
	constants.vertex_index_offset = m_vertex_index_offset;
	for (u32 unit = 0; unit < 4; ++unit)
	{
		const auto& texture = rsx::method_registers.vertex_textures[unit];
		const float logical[] = {float(texture.width()), float(texture.height()), float(std::max<u16>(1, texture.depth()))};
		float physical[] = {logical[0], logical[1], logical[2]};
		if (texture.enabled())
		{
			const auto address = rsx::get_address(texture.offset(), texture.location());
			auto* target = m_rtts.get_texture_from_render_target_if_applicable(address);
			if (!target) target = m_rtts.get_texture_from_depth_stencil_if_applicable(address);
			if (target)
			{
				physical[0] = float(target->GetDesc().Width);
				physical[1] = float(target->GetDesc().Height);
				if (target->GetDesc().SampleDesc.Count > 1)
				{
					physical[0] *= 2;
					physical[1] *= target->GetDesc().SampleDesc.Count / 2;
				}
			}
		}
		const bool unnormalized = (texture.format() & CELL_GCM_TEXTURE_UN) != 0;
		for (u32 axis = 0; axis < 3; ++axis)
			constants.vertex_texture_scale[unit][axis] = (unnormalized ? 1.f : logical[axis]) / std::max(1.f, physical[axis]);
	}
	std::memcpy(mapped_buffer, &constants, sizeof(constants));
	m_buffer_data.unmap(CD3DX12_RANGE(heap_offset, heap_offset + allocation_size));

	D3D12_CONSTANT_BUFFER_VIEW_DESC constant_buffer_view_desc = {
		m_buffer_data.get_heap()->GetGPUVirtualAddress() + heap_offset,
		allocation_size
	};
	m_device->CreateConstantBufferView(&constant_buffer_view_desc,
		CD3DX12_CPU_DESCRIPTOR_HANDLE(get_current_resource_storage().descriptors_heap->GetCPUDescriptorHandleForHeapStart())
		.Offset((INT)descriptorIndex, m_descriptor_stride_srv_cbv_uav));
}

void D3D12GSRender::upload_and_bind_vertex_shader_constants(size_t descriptor_index)
{
	size_t buffer_size = 512 * 4 * sizeof(float);

	size_t heap_offset = m_buffer_data.alloc<D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT>(buffer_size);

	void *mapped_buffer = m_buffer_data.map<void>(CD3DX12_RANGE(heap_offset, heap_offset + buffer_size));
	std::memset(mapped_buffer, 0, buffer_size);
	GRAPH_frontend().fill_vertex_program_constants_data(mapped_buffer, {});
	*(reinterpret_cast<u32*>((char *)mapped_buffer + (468 * 4 * sizeof(float)))) = rsx::method_registers.transform_branch_bits();
	m_buffer_data.unmap(CD3DX12_RANGE(heap_offset, heap_offset + buffer_size));

	D3D12_CONSTANT_BUFFER_VIEW_DESC constant_buffer_view_desc = {
		m_buffer_data.get_heap()->GetGPUVirtualAddress() + heap_offset,
		(UINT)buffer_size
	};
	m_device->CreateConstantBufferView(&constant_buffer_view_desc,
		CD3DX12_CPU_DESCRIPTOR_HANDLE(get_current_resource_storage().descriptors_heap->GetCPUDescriptorHandleForHeapStart())
		.Offset((INT)descriptor_index, m_descriptor_stride_srv_cbv_uav));
}

D3D12_CONSTANT_BUFFER_VIEW_DESC D3D12GSRender::upload_fragment_shader_constants()
{
	// Get constant from fragment program
	ensure(m_current_fragment_shader);
	size_t buffer_size = std::max<size_t>(16, m_current_fragment_shader->constant_offsets.size() * 16);
	// Multiple of 256 never 0
	buffer_size = (buffer_size + 255) & ~255;

	size_t heap_offset = m_buffer_data.alloc<D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT>(buffer_size);

	size_t offset = 0;
	float *mapped_buffer = m_buffer_data.map<float>(CD3DX12_RANGE(heap_offset, heap_offset + buffer_size));
	std::memset(mapped_buffer, 0, buffer_size);
	m_pso_cache.fill_fragment_constants_buffer({ mapped_buffer, buffer_size / sizeof(float) }, *m_current_fragment_shader, current_fragment_program);
	m_buffer_data.unmap(CD3DX12_RANGE(heap_offset, heap_offset + buffer_size));

	return {
		m_buffer_data.get_heap()->GetGPUVirtualAddress() + heap_offset,
		(UINT)buffer_size
	};
}
std::tuple<bool, size_t, std::vector<D3D12_SHADER_RESOURCE_VIEW_DESC>>
D3D12GSRender::upload_and_set_vertex_index_data(ID3D12GraphicsCommandList* command_list)
{
    auto& clause = rsx::method_registers.current_draw_clause;
    rsx::vertex_input_layout layout;
    GRAPH_frontend().analyse_inputs_interleaved(layout, current_vp_metadata);
    u32 count = clause.command == rsx::draw_command::inlined_array
        ? 0 : clause.get_elements_count();
    u32 vertex_count = count;
    bool indexed = clause.command == rsx::draw_command::indexed;
    m_vertex_index_offset = indexed || clause.command == rsx::draw_command::inlined_array
        ? 0 : clause.min_index();
    if (clause.command == rsx::draw_command::inlined_array)
    {
        ensure(!layout.interleaved_blocks.empty());
        const u32 stride = layout.interleaved_blocks[0]->attribute_stride;
        ensure(stride && clause.inline_vertex_array.size_bytes() % stride == 0);
        count = vertex_count = ::narrow<u32>(clause.inline_vertex_array.size_bytes() / stride);
    }
    if (indexed)
    {
        const auto source = GRAPH_frontend().get_raw_index_array(clause);
        const auto type = clause.is_immediate_draw ? rsx::index_array_type::u32 : rsx::method_registers.index_type();
        const u32 index_size = get_index_type_size(type);
        const auto bytes = utils::align(static_cast<usz>(get_index_count(clause.primitive, count)) * index_size, 256u);
        const auto offset = m_buffer_data.alloc<256>(std::max<usz>(bytes, 256));
        auto* data = m_buffer_data.map<std::byte>(offset);
        std::memset(data, 0, bytes);
        const auto [min_index, max_index, written] = write_index_array_data_to_buffer(
            {data, bytes}, source, type, clause.primitive,
            rsx::method_registers.restart_index_enabled(), rsx::method_registers.restart_index(),
            [](auto primitive) { return !is_primitive_native(primitive); });
        count = written;
        if (!count)
        {
            m_buffer_data.unmap(CD3DX12_RANGE(offset, offset + bytes));
            return {true, 0, {}};
        }
        ensure(max_index != umax);
        vertex_count = max_index + 1;
        m_buffer_data.unmap(CD3DX12_RANGE(offset, offset + bytes));
        const D3D12_INDEX_BUFFER_VIEW view{m_buffer_data.get_heap()->GetGPUVirtualAddress() + offset,
            ::narrow<UINT>(bytes), get_index_type(type)};
        command_list->IASetIndexBuffer(&view);
    }
    else
    {
        ensure(vertex_count <= UINT32_MAX - m_vertex_index_offset);
        vertex_count += m_vertex_index_offset;
        if (!is_primitive_native(clause.primitive))
        {
            ensure(count <= 65535, "Legacy primitive expansion exceeds its 16-bit index range");
            const u32 expanded = get_index_count(clause.primitive, count);
            const usz bytes = utils::align(static_cast<usz>(expanded) * sizeof(u16), 256u);
            const auto offset = m_buffer_data.alloc<256>(std::max<usz>(bytes, 256));
            auto* data = m_buffer_data.map<char>(offset);
            std::memset(data, 0, bytes);
            write_index_array_for_non_indexed_non_native_primitive_to_buffer(data, clause.primitive, count);
            m_buffer_data.unmap(CD3DX12_RANGE(offset, offset + bytes));
            const D3D12_INDEX_BUFFER_VIEW view{m_buffer_data.get_heap()->GetGPUVirtualAddress() + offset,
                ::narrow<UINT>(bytes), DXGI_FORMAT_R16_UINT};
            command_list->IASetIndexBuffer(&view);
            indexed = true;
            count = expanded;
        }
    }
    if (!count) return {indexed, 0, {}};
    const auto [persistent_bytes, volatile_bytes] = calculate_memory_requirements(layout, 0, vertex_count);
    const usz volatile_base = utils::align(persistent_bytes, 4u);
    const usz total = utils::align(volatile_base + volatile_bytes, 4u);
    ensure(total && total <= m_vertex_buffer_data->GetDesc().Width, "D3D12 vertex stream exceeds GPU buffer");
    const auto offset = m_buffer_data.alloc<256>(total);
    auto* data = m_buffer_data.map<std::byte>(offset);
    std::memset(data, 0, total);
    GRAPH_frontend().write_vertex_data_to_memory(layout, 0, vertex_count,
        persistent_bytes ? data : nullptr, volatile_bytes ? data + volatile_base : nullptr);
    m_vertex_layout_state.fill(0);
    GRAPH_frontend().fill_vertex_layout_state(layout, current_vp_metadata, 0, vertex_count,
        m_vertex_layout_state.data(), 0, ::narrow<u32>(volatile_base));
    m_buffer_data.unmap(CD3DX12_RANGE(offset, offset + total));
    const auto before = CD3DX12_RESOURCE_BARRIER::Transition(m_vertex_buffer_data.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list->ResourceBarrier(1, &before);
    command_list->CopyBufferRegion(m_vertex_buffer_data.Get(), 0, m_buffer_data.get_heap(), offset, total);
    const auto after = CD3DX12_RESOURCE_BARRIER::Transition(m_vertex_buffer_data.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command_list->ResourceBarrier(1, &after);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = ::narrow<UINT>(total / 4);
    view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    return {indexed, count, {view}};
}

#endif
