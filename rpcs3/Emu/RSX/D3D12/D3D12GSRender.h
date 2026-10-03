#pragma once

#include "D3D12Utils.h"
#include "Emu/Memory/vm.h"
#include "Emu/System.h"
#include "Emu/RSX/GSRender.h"

#include "D3D12RenderTargetSets.h"
#include "D3D12PipelineState.h"
#include "d3dx12.h"
#include "D3D12MemoryHelpers.h"
#include "D3D12MSAA.h"
#include <future>
#include <deque>
#include "D3D12CpuAccess.h"


// Experimental current-RSX backend. See PORTING.md for tested capabilities,
// sample-pattern differences and remaining implicit surface-coherence limits.

/**
 * Structure used to load/unload D3D12 lib.
 */
struct D3D12DLLManagement
{
	D3D12DLLManagement();
	~D3D12DLLManagement();
};

class D3D12GSRender : public GSRender
{
private:
	/** D3D12 structures.
	 * Note: they should be declared in reverse order of destruction
	 */
	D3D12DLLManagement m_d3d12_lib;
	ComPtr<ID3D12Device> m_device;
	ComPtr<ID3D12CommandQueue> m_command_queue;
	ComPtr<struct IDXGISwapChain3> m_swap_chain;
	u32 m_frame_index = 0;
	ComPtr<ID3D12Resource> m_backbuffer[2];
	ComPtr<ID3D12DescriptorHeap> m_backbuffer_descriptor_heap[2];

	ComPtr<ID3D12RootSignature> m_shared_root_signature;

	// TODO: Use a tree structure to parse more efficiently
	d3d12::guest_pages m_guest_pages;
	data_cache m_texture_cache;
	d3d12::sample_pipeline_cache m_sample_pipeline_cache;
	d3d12::sample_pattern m_active_sample_pattern = d3d12::sample_pattern::center;
	bool invalidate_address(u32 addr);
	std::mutex m_surface_invalidation_mutex;
	std::vector<std::pair<u32, u32>> m_surface_invalidations;
	void apply_surface_invalidations();
	struct watched_surface
	{
		ComPtr<ID3D12Resource> resource;
		u32 address, pitch;
		rsx::surface_color_format color;
		rsx::surface_depth_format2 depth;
		bool is_depth, dirty;
	};
	std::unordered_map<u64, watched_surface> m_watched_surfaces;
	d3d12::cpu_access_queue m_cpu_access_queue;
	void watch_bound_surfaces(bool dirty = true);
	void restore_bound_surfaces();
	void process_surface_requests();
	void synchronize_surface_memory(u32 address, u32 length, bool write);
	bool request_surface_memory(u32 address, u32 length, bool write, bool lease = false);

	PipelineStateObjectCache m_pso_cache;
	std::tuple<ComPtr<ID3D12PipelineState>, size_t, size_t> m_current_pso;
	const Shader* m_current_vertex_shader = nullptr;
	const Shader* m_current_fragment_shader = nullptr;
	std::array<s32, 32> m_vertex_layout_state{};
	u32 m_vertex_index_offset = 0;
	void emit_draw();

	struct
	{
		size_t draw_calls_duration;
		size_t draw_calls_count;
		size_t prepare_rtt_duration;
		size_t vertex_index_duration;
		size_t buffer_upload_size;
		size_t program_load_duration;
		size_t constants_duration;
		size_t texture_duration;
		size_t flip_duration;
	} m_timers;

	void reset_timer();

	struct shader
	{
		ID3D12PipelineState *pso;
		ID3D12RootSignature *root_signature;
		ID3D12Resource *vertex_buffer;
		ID3D12DescriptorHeap *texture_descriptor_heap;
		ID3D12DescriptorHeap *sampler_descriptor_heap;
		void init(ID3D12Device *device, ID3D12CommandQueue *gfx_command_queue);
		void release();
	};

	/**
	 * Stores data related to the scaling pass that turns internal
	 * render targets into presented buffers.
	 */
	shader m_output_scaling_pass;

	resource_storage m_per_frame_storage[2];
	resource_storage &get_current_resource_storage();
	resource_storage &get_non_current_resource_storage();

	// Textures, constants, index and vertex buffers storage
	d3d12_data_heap m_buffer_data;
	d3d12_data_heap m_readback_resources;
	ComPtr<ID3D12Resource> m_vertex_buffer_data;

	rsx::render_targets m_rtts;

	INT m_descriptor_stride_srv_cbv_uav;
	INT m_descriptor_stride_dsv;
	INT m_descriptor_stride_rtv;
	INT m_descriptor_stride_samplers;

	// Used to fill unused texture slot
	ID3D12Resource *m_dummy_texture;

	// Currently used shader resources / samplers descriptor
	u32 m_current_transform_constants_buffer_descriptor_id;
	ComPtr<ID3D12DescriptorHeap> m_current_texture_descriptors;
	ComPtr<ID3D12DescriptorHeap> m_current_sampler_descriptors;
	ComPtr<ID3D12DescriptorHeap> m_current_vertex_texture_descriptors;
	ComPtr<ID3D12DescriptorHeap> m_current_vertex_sampler_descriptors;

public:
	u64 get_cycles() override final;
	explicit D3D12GSRender(utils::serial* ar = nullptr);
	virtual ~D3D12GSRender();

private:
	void init_d2d_structures();
	void release_d2d_structures();

	void load_program();

	void set_rtt_and_ds(ID3D12GraphicsCommandList *command_list);

	/**
	 * Create vertex and index buffers (if needed) and set them to cmdlist.
	 * Non native primitive type are emulated by index buffers expansion.
	 * Returns whether the draw call is indexed or not and the vertex count to draw.
	*/
	std::tuple<bool, size_t, std::vector<D3D12_SHADER_RESOURCE_VIEW_DESC> > upload_and_set_vertex_index_data(ID3D12GraphicsCommandList *command_list);

	void upload_and_bind_scale_offset_matrix(size_t descriptor_index);
	void upload_and_bind_vertex_shader_constants(size_t descriptor_index);
	D3D12_CONSTANT_BUFFER_VIEW_DESC upload_fragment_shader_constants();
	/**
	 * Fetch all textures recorded in the state in the render target cache and in the texture cache.
	 * If a texture is not cached, populate cmdlist with uploads command.
	 * Create necessary resource view/sampler descriptors in the per frame storage struct.
	 * If the count of enabled texture is below texture_count, fills with dummy texture and sampler.
	 */
	void upload_textures(ID3D12GraphicsCommandList *command_list, size_t texture_count);
	void upload_vertex_textures(ID3D12GraphicsCommandList* command_list);
	template <typename Texture>
	void upload_texture_bank(ID3D12GraphicsCommandList* command_list, size_t texture_count,
		Texture* textures, ID3D12DescriptorHeap* texture_descriptors, ID3D12DescriptorHeap* sampler_descriptors);

	/**
	 * Creates render target if necessary.
	 * Populate cmdlist with render target state change (from RTT to generic read for previous rtt,
	 * from generic to rtt for rtt in cache).
	 */
	void prepare_render_targets(ID3D12GraphicsCommandList *command_list);

	/**
	 * Render D2D overlay if enabled on top of the backbuffer.
	 */
	void render_overlay();

	void clear_surface(u32 arg);

	/**
	 * Copy currently bound current target to the dma location affecting them.
	 * NOTE: We should also copy previously bound rtts.
	 */
	void copy_render_target_to_dma_location();

protected:
	virtual void on_init_thread() override;
	virtual void on_exit() override;
	virtual void do_local_task(rsx::FIFO::state state) override;
	bool release_GCM_label(u32 type, u32 address, u32 value) override;
	void on_semaphore_acquire_wait() override;
	void write_barrier(u32 address, u32 range) override;
	bool begin_cpu_memory_access(u32 address, u32 length) override;
	void end_cpu_memory_access() noexcept override;
	void on_invalidate_memory_range(const utils::address_range32& range, rsx::invalidation_cause cause) override;
	rsx::flags32_t read_barrier(u32 address, u32 range, bool unconditional) override;
	virtual void end() override;
	virtual void flip(const rsx::display_flip_info_t& info) override;

	virtual bool on_access_violation(u32 address, bool is_writing) override;

	std::array<std::vector<std::byte>, 4> copy_render_targets_to_memory();
	std::array<std::vector<std::byte>, 2> copy_depth_stencil_buffer_to_memory();
	virtual std::pair<std::string, std::string> get_programs() const override;
	virtual void notify_tile_unbound(u32 tile) override;
};
