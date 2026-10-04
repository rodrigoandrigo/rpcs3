#pragma once

#include "UwpImGuiFrontend/Types.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <winrt/Windows.UI.Core.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace UwpImGuiFrontend::Sample
{
class D3D12Renderer
{
public:
	D3D12Renderer() = default;
	~D3D12Renderer();

	D3D12Renderer(const D3D12Renderer&) = delete;
	D3D12Renderer& operator=(const D3D12Renderer&) = delete;

	void Initialize(const winrt::Windows::UI::Core::CoreWindow& window,
		std::uint32_t width, std::uint32_t height);
	// XAML host path. swapChainPanel is the ABI IInspectable/IUnknown for a
	// Windows.UI.Xaml.Controls.SwapChainPanel.
	void Initialize(IUnknown* swapChainPanel,
		std::uint32_t width, std::uint32_t height);
	void Shutdown();
	void Resize(std::uint32_t width, std::uint32_t height);
	void SetCompositionScale(float x, float y);
	void BeginFrame(float deltaSeconds);
	void EndFrame();
	void WaitForGpu();
	[[nodiscard]] ID3D12Device* Device() const noexcept { return m_device.Get(); }
	[[nodiscard]] ID3D12CommandQueue* Queue() const noexcept { return m_queue.Get(); }
	// Call between BeginFrame and EndFrame. Retains the immutable core snapshot
	// and its per-swapchain-slot SRV until the host fence for that slot completes.
	[[nodiscard]] TextureHandle ImportCoreFrame(ID3D12Resource* resource);

	[[nodiscard]] TextureHandle LoadTexture(const std::filesystem::path& path);
	void ReleaseTexture(TextureHandle texture);
	[[nodiscard]] std::uint32_t Width() const noexcept { return m_width; }
	[[nodiscard]] std::uint32_t Height() const noexcept { return m_height; }

private:
	static constexpr std::uint32_t kFrameCount = 3;
	static constexpr std::uint32_t kDescriptorCount = 256;

	struct FrameResources
	{
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
		Microsoft::WRL::ComPtr<ID3D12Resource> target;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
		std::uint64_t fenceValue = 0;
		Microsoft::WRL::ComPtr<ID3D12Resource> coreFrame;
		std::uint32_t coreDescriptor = 0;
	};

	struct TextureResource
	{
		Microsoft::WRL::ComPtr<ID3D12Resource> resource;
		std::uint32_t descriptorIndex = 0;
	};

	void CreateDeviceResources();
	void CreateWindowResources(const winrt::Windows::UI::Core::CoreWindow& window);
	void CreatePanelResources(IUnknown* swapChainPanel);
	void CreateRenderTargets();
	void ReleaseRenderTargets();
	void WaitForFence(std::uint64_t value);
	[[nodiscard]] std::uint32_t AllocateDescriptor();
	void FreeDescriptor(std::uint32_t index);
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptor(std::uint32_t index) const;
	[[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE GpuDescriptor(std::uint32_t index) const;

	winrt::Windows::UI::Core::CoreWindow m_window{ nullptr };
	Microsoft::WRL::ComPtr<IDXGIFactory4> m_factory;
	Microsoft::WRL::ComPtr<ID3D12Device> m_device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
	Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
	Microsoft::WRL::ComPtr<IUnknown> m_swapChainPanel;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_uploadAllocator;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_uploadCommandList;
	Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
	std::array<FrameResources, kFrameCount> m_frames;
	std::unordered_map<std::uintptr_t, TextureResource> m_textures;
	std::vector<std::uint32_t> m_freeDescriptors;
	HANDLE m_fenceEvent = nullptr;
	std::uint64_t m_nextFenceValue = 1;
	std::uint32_t m_rtvIncrement = 0;
	std::uint32_t m_srvIncrement = 0;
	std::uint32_t m_width = 1;
	std::uint32_t m_height = 1;
	bool m_imguiBackendInitialized = false;
};
}
