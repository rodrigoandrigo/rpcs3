#include "D3D12Renderer.h"
#include "CoreVideoMapping.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <wincodec.h>
#include <windows.ui.xaml.media.dxinterop.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace UwpImGuiFrontend::Sample
{
namespace
{
using Microsoft::WRL::ComPtr;

void Check(HRESULT result, const char* operation)
{
	if (FAILED(result))
		throw std::runtime_error(std::string(operation) + " failed");
}

struct DecodedImage
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<std::uint8_t> pixels;
};

DecodedImage DecodeImage(const std::filesystem::path& path)
{
	ComPtr<IWICImagingFactory> factory;
	Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(factory.GetAddressOf())), "Create WIC factory");

	ComPtr<IWICBitmapDecoder> decoder;
	Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()), "Decode image");
	ComPtr<IWICBitmapFrameDecode> frame;
	Check(decoder->GetFrame(0, frame.GetAddressOf()), "Read image frame");

	DecodedImage image;
	Check(frame->GetSize(&image.width, &image.height), "Read image dimensions");
	if (image.width == 0 || image.height == 0 ||
		image.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
		image.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
	{
		throw std::runtime_error("Image dimensions are not supported");
	}

	ComPtr<IWICFormatConverter> converter;
	Check(factory->CreateFormatConverter(converter.GetAddressOf()),
		"Create image converter");
	Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
		WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
		"Convert image");
	const std::uint64_t rowBytes = static_cast<std::uint64_t>(image.width) * 4;
	const std::uint64_t byteCount = rowBytes * image.height;
	if (rowBytes > std::numeric_limits<UINT>::max() ||
		byteCount > std::numeric_limits<UINT>::max())
	{
		throw std::runtime_error("Decoded image is too large");
	}
	image.pixels.resize(static_cast<std::size_t>(byteCount));
	Check(converter->CopyPixels(nullptr, static_cast<UINT>(rowBytes),
		static_cast<UINT>(byteCount), image.pixels.data()), "Copy image pixels");
	return image;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	return barrier;
}
}

D3D12Renderer::~D3D12Renderer()
{
	Shutdown();
}

void D3D12Renderer::Initialize(
	const winrt::Windows::UI::Core::CoreWindow& window,
	std::uint32_t width, std::uint32_t height)
{
	if (m_device)
		return;
	m_window = window;
	m_width = std::max(1u, width);
	m_height = std::max(1u, height);
	CreateDeviceResources();
	CreateWindowResources(window);

	if (!ImGui_ImplDX12_Init(m_device.Get(), kFrameCount,
		DXGI_FORMAT_R8G8B8A8_UNORM, m_srvHeap.Get(), CpuDescriptor(0),
		GpuDescriptor(0)))
	{
		throw std::runtime_error("Initialize ImGui Direct3D 12 backend failed");
	}
	m_imguiBackendInitialized = true;
}

void D3D12Renderer::Initialize(IUnknown* swapChainPanel,
	std::uint32_t width, std::uint32_t height)
{
	if (m_device)
		return;
	if (!swapChainPanel)
		throw std::invalid_argument("SwapChainPanel is required");
	m_width = std::max(1u, width);
	m_height = std::max(1u, height);
	CreateDeviceResources();
	CreatePanelResources(swapChainPanel);
	if (!ImGui_ImplDX12_Init(m_device.Get(), kFrameCount,
		DXGI_FORMAT_R8G8B8A8_UNORM, m_srvHeap.Get(), CpuDescriptor(0),
		GpuDescriptor(0)))
	{
		throw std::runtime_error("Initialize ImGui Direct3D 12 backend failed");
	}
	m_imguiBackendInitialized = true;
}

void D3D12Renderer::Shutdown()
{
	if (!m_device)
		return;
	WaitForGpu();
	m_textures.clear();
	if (m_imguiBackendInitialized)
	{
		ImGui_ImplDX12_Shutdown();
		m_imguiBackendInitialized = false;
	}
	ReleaseRenderTargets();
	if (m_swapChainPanel)
	{
		ComPtr<ISwapChainPanelNative> panel;
		if (SUCCEEDED(m_swapChainPanel.As(&panel)))
			(void)panel->SetSwapChain(nullptr);
	}
	m_uploadCommandList.Reset();
	m_uploadAllocator.Reset();
	m_commandList.Reset();
	m_swapChain.Reset();
	m_swapChainPanel.Reset();
	m_srvHeap.Reset();
	m_rtvHeap.Reset();
	m_queue.Reset();
	m_fence.Reset();
	m_device.Reset();
	m_factory.Reset();
	if (m_fenceEvent)
	{
		CloseHandle(m_fenceEvent);
		m_fenceEvent = nullptr;
	}
	m_window = nullptr;
}

void D3D12Renderer::CreateDeviceResources()
{
	UINT factoryFlags = 0;
#if defined(_DEBUG)
	ComPtr<ID3D12Debug> debug;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf()))))
	{
		debug->EnableDebugLayer();
		factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
	}
#endif
	Check(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(m_factory.GetAddressOf())),
		"Create DXGI factory");
	Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(m_device.GetAddressOf())), "Create Direct3D 12 device");

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	Check(m_device->CreateCommandQueue(&queueDesc,
		IID_PPV_ARGS(m_queue.GetAddressOf())), "Create command queue");

	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.NumDescriptors = kFrameCount;
	Check(m_device->CreateDescriptorHeap(&rtvDesc,
		IID_PPV_ARGS(m_rtvHeap.GetAddressOf())), "Create render-target heap");
	m_rtvIncrement = m_device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
	srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvDesc.NumDescriptors = kDescriptorCount;
	srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	Check(m_device->CreateDescriptorHeap(&srvDesc,
		IID_PPV_ARGS(m_srvHeap.GetAddressOf())), "Create texture heap");
	m_srvIncrement = m_device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	for (std::uint32_t index = kDescriptorCount; index-- > 1;)
		m_freeDescriptors.push_back(index);

	for (FrameResources& frame : m_frames)
	{
		Check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(frame.allocator.GetAddressOf())),
			"Create frame command allocator");
	}
	Check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		m_frames.front().allocator.Get(), nullptr,
		IID_PPV_ARGS(m_commandList.GetAddressOf())), "Create command list");
	Check(m_commandList->Close(), "Close command list");

	Check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(m_uploadAllocator.GetAddressOf())),
		"Create upload command allocator");
	Check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		m_uploadAllocator.Get(), nullptr,
		IID_PPV_ARGS(m_uploadCommandList.GetAddressOf())),
		"Create upload command list");
	Check(m_uploadCommandList->Close(), "Close upload command list");

	Check(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(m_fence.GetAddressOf())), "Create fence");
	m_fenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
	if (!m_fenceEvent)
		throw std::runtime_error("Create fence event failed");
}

void D3D12Renderer::CreateWindowResources(
	const winrt::Windows::UI::Core::CoreWindow& window)
{
	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.Width = m_width;
	desc.Height = m_height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = kFrameCount;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.Scaling = DXGI_SCALING_STRETCH;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

	ComPtr<IDXGISwapChain1> swapChain;
	Check(m_factory->CreateSwapChainForCoreWindow(m_queue.Get(),
		reinterpret_cast<IUnknown*>(winrt::get_abi(window)), &desc, nullptr,
		swapChain.GetAddressOf()), "Create CoreWindow swap chain");
	Check(swapChain.As(&m_swapChain), "Query swap chain 3");
	CreateRenderTargets();
}

void D3D12Renderer::CreatePanelResources(IUnknown* swapChainPanel)
{
	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.Width = m_width;
	desc.Height = m_height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = kFrameCount;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.Scaling = DXGI_SCALING_STRETCH;
	desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

	ComPtr<IDXGISwapChain1> swapChain;
	Check(m_factory->CreateSwapChainForComposition(m_queue.Get(), &desc, nullptr,
		swapChain.GetAddressOf()), "Create SwapChainPanel composition swap chain");
	Check(swapChain.As(&m_swapChain), "Query swap chain 3");
	Check(swapChainPanel->QueryInterface(IID_PPV_ARGS(m_swapChainPanel.GetAddressOf())),
		"Retain SwapChainPanel");
	ComPtr<ISwapChainPanelNative> panel;
	Check(m_swapChainPanel.As(&panel), "Query ISwapChainPanelNative");
	Check(panel->SetSwapChain(m_swapChain.Get()), "Attach swap chain to SwapChainPanel");
	CreateRenderTargets();
}

void D3D12Renderer::CreateRenderTargets()
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle =
		m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (std::uint32_t index = 0; index < kFrameCount; ++index)
	{
		FrameResources& frame = m_frames[index];
		Check(m_swapChain->GetBuffer(index, IID_PPV_ARGS(frame.target.GetAddressOf())),
			"Get swap-chain buffer");
		frame.rtv = handle;
		m_device->CreateRenderTargetView(frame.target.Get(), nullptr, handle);
		handle.ptr += m_rtvIncrement;
	}
}

void D3D12Renderer::ReleaseRenderTargets()
{
	for (FrameResources& frame : m_frames)
	{
		frame.target.Reset();
		frame.coreFrame.Reset();
		frame.fenceValue = 0;
	}
}

void D3D12Renderer::Resize(std::uint32_t width, std::uint32_t height)
{
	width = std::max(1u, width);
	height = std::max(1u, height);
	if (!m_swapChain || (m_width == width && m_height == height))
	{
		m_width = width;
		m_height = height;
		return;
	}
	WaitForGpu();
	ReleaseRenderTargets();
	Check(m_swapChain->ResizeBuffers(kFrameCount, width, height,
		DXGI_FORMAT_R8G8B8A8_UNORM, 0), "Resize swap chain");
	m_width = width;
	m_height = height;
	CreateRenderTargets();
}

void D3D12Renderer::SetCompositionScale(float x, float y)
{
	if (!m_swapChainPanel || !(x > 0.0f) || !(y > 0.0f))
		return;
	DXGI_MATRIX_3X2_F transform{};
	transform._11 = 1.0f / x;
	transform._22 = 1.0f / y;
	Check(m_swapChain->SetMatrixTransform(&transform), "Set panel composition scale");
}

void D3D12Renderer::BeginFrame(float deltaSeconds)
{
	WaitForFence(m_frames[m_swapChain->GetCurrentBackBufferIndex()].fenceValue);
	ImGui_ImplDX12_NewFrame();
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = { static_cast<float>(m_width), static_cast<float>(m_height) };
	io.DeltaTime = std::max(deltaSeconds, 1.0f / 1000.0f);
	ImGui::NewFrame();
}

TextureHandle D3D12Renderer::ImportCoreFrame(ID3D12Resource* resource)
{
	if (!resource) return {};
	const auto desc = resource->GetDesc();
	if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
		!IsCoreVideoFormat(desc.Format) || desc.SampleDesc.Count != 1)
		throw std::runtime_error("Unsupported RPCS3 frame texture");
	Microsoft::WRL::ComPtr<ID3D12Device> owner;
	Check(resource->GetDevice(IID_PPV_ARGS(owner.GetAddressOf())), "Query RPCS3 frame device");
	if (owner.Get() != m_device.Get()) throw std::runtime_error("RPCS3 frame belongs to a different device");
	auto& frame = m_frames[m_swapChain->GetCurrentBackBufferIndex()];
	if (!frame.coreDescriptor) frame.coreDescriptor = AllocateDescriptor();
	frame.coreFrame = resource;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = desc.Format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = CoreVideoComponentMapping;
	srv.Texture2D.MipLevels = 1;
	m_device->CreateShaderResourceView(resource, &srv, CpuDescriptor(frame.coreDescriptor));
	return {static_cast<std::uintptr_t>(GpuDescriptor(frame.coreDescriptor).ptr),
		static_cast<std::uint32_t>(desc.Width), desc.Height};
}

void D3D12Renderer::EndFrame()
{
	ImGui::Render();
	const std::uint32_t frameIndex = m_swapChain->GetCurrentBackBufferIndex();
	FrameResources& frame = m_frames[frameIndex];
	WaitForFence(frame.fenceValue);
	Check(frame.allocator->Reset(), "Reset command allocator");
	Check(m_commandList->Reset(frame.allocator.Get(), nullptr),
		"Reset command list");

	const auto toRenderTarget = Transition(frame.target.Get(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_commandList->ResourceBarrier(1, &toRenderTarget);
	constexpr float clearColor[4]{ 0.025f, 0.028f, 0.035f, 1.0f };
	m_commandList->ClearRenderTargetView(frame.rtv, clearColor, 0, nullptr);
	m_commandList->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
	ID3D12DescriptorHeap* heaps[]{ m_srvHeap.Get() };
	m_commandList->SetDescriptorHeaps(1, heaps);
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());
	const auto toPresent = Transition(frame.target.Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	m_commandList->ResourceBarrier(1, &toPresent);
	Check(m_commandList->Close(), "Close frame command list");
	ID3D12CommandList* lists[]{ m_commandList.Get() };
	m_queue->ExecuteCommandLists(1, lists);
	Check(m_swapChain->Present(1, 0), "Present frame");
	frame.fenceValue = m_nextFenceValue++;
	Check(m_queue->Signal(m_fence.Get(), frame.fenceValue), "Signal frame fence");
}

void D3D12Renderer::WaitForFence(std::uint64_t value)
{
	if (value == 0 || m_fence->GetCompletedValue() >= value)
		return;
	Check(m_fence->SetEventOnCompletion(value, m_fenceEvent),
		"Set fence event");
	if (WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE) != WAIT_OBJECT_0)
		throw std::runtime_error("Wait for GPU fence failed");
}

void D3D12Renderer::WaitForGpu()
{
	if (!m_queue || !m_fence)
		return;
	const std::uint64_t value = m_nextFenceValue++;
	Check(m_queue->Signal(m_fence.Get(), value), "Signal GPU fence");
	WaitForFence(value);
}

std::uint32_t D3D12Renderer::AllocateDescriptor()
{
	if (m_freeDescriptors.empty())
		throw std::runtime_error("The sample texture descriptor heap is full");
	const std::uint32_t index = m_freeDescriptors.back();
	m_freeDescriptors.pop_back();
	return index;
}

void D3D12Renderer::FreeDescriptor(std::uint32_t index)
{
	if (index > 0 && index < kDescriptorCount)
		m_freeDescriptors.push_back(index);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::CpuDescriptor(
	std::uint32_t index) const
{
	auto handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += static_cast<SIZE_T>(index) * m_srvIncrement;
	return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::GpuDescriptor(
	std::uint32_t index) const
{
	auto handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
	handle.ptr += static_cast<UINT64>(index) * m_srvIncrement;
	return handle;
}

TextureHandle D3D12Renderer::LoadTexture(const std::filesystem::path& path)
{
	if (path.empty() || !std::filesystem::exists(path))
		return {};
	std::uint32_t descriptorIndex = 0;
	try
	{
		const DecodedImage image = DecodeImage(path);
		descriptorIndex = AllocateDescriptor();
		ComPtr<ID3D12Resource> texture;
		ComPtr<ID3D12Resource> upload;

		D3D12_RESOURCE_DESC textureDesc{};
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		textureDesc.Width = image.width;
		textureDesc.Height = image.height;
		textureDesc.DepthOrArraySize = 1;
		textureDesc.MipLevels = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		D3D12_HEAP_PROPERTIES defaultHeap{};
		defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
		Check(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
			&textureDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(texture.GetAddressOf())), "Create texture");

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		UINT rows = 0;
		UINT64 rowSize = 0;
		UINT64 uploadSize = 0;
		m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint,
			&rows, &rowSize, &uploadSize);
		D3D12_RESOURCE_DESC uploadDesc{};
		uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		uploadDesc.Width = uploadSize;
		uploadDesc.Height = 1;
		uploadDesc.DepthOrArraySize = 1;
		uploadDesc.MipLevels = 1;
		uploadDesc.SampleDesc.Count = 1;
		uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		D3D12_HEAP_PROPERTIES uploadHeap{};
		uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
		Check(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
			&uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(upload.GetAddressOf())), "Create texture upload buffer");

		std::uint8_t* mapped = nullptr;
		D3D12_RANGE readRange{ 0, 0 };
		Check(upload->Map(0, &readRange, reinterpret_cast<void**>(&mapped)),
			"Map texture upload buffer");
		const std::size_t sourcePitch = static_cast<std::size_t>(image.width) * 4;
		for (std::uint32_t row = 0; row < image.height; ++row)
		{
			std::memcpy(mapped + footprint.Offset +
				static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
				image.pixels.data() + static_cast<std::size_t>(row) * sourcePitch,
				sourcePitch);
		}
		D3D12_RANGE writeRange{ footprint.Offset,
			static_cast<SIZE_T>(footprint.Offset + uploadSize) };
		upload->Unmap(0, &writeRange);

		WaitForGpu();
		Check(m_uploadAllocator->Reset(), "Reset upload allocator");
		Check(m_uploadCommandList->Reset(m_uploadAllocator.Get(), nullptr),
			"Reset upload command list");
		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = upload.Get();
		source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint = footprint;
		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = texture.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destination.SubresourceIndex = 0;
		m_uploadCommandList->CopyTextureRegion(&destination, 0, 0, 0, &source,
			nullptr);
		const auto ready = Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_uploadCommandList->ResourceBarrier(1, &ready);
		Check(m_uploadCommandList->Close(), "Close upload command list");
		ID3D12CommandList* lists[]{ m_uploadCommandList.Get() };
		m_queue->ExecuteCommandLists(1, lists);
		WaitForGpu();

		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = textureDesc.Format;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		m_device->CreateShaderResourceView(texture.Get(), &srv,
			CpuDescriptor(descriptorIndex));
		const std::uintptr_t id = static_cast<std::uintptr_t>(
			GpuDescriptor(descriptorIndex).ptr);
		m_textures.emplace(id, TextureResource{ std::move(texture), descriptorIndex });
		descriptorIndex = 0;
		return { id, image.width, image.height };
	}
	catch (...)
	{
		if (descriptorIndex != 0)
			FreeDescriptor(descriptorIndex);
		return {};
	}
}

void D3D12Renderer::ReleaseTexture(TextureHandle texture)
{
	const auto found = m_textures.find(texture.id);
	if (found == m_textures.end())
		return;
	WaitForGpu();
	FreeDescriptor(found->second.descriptorIndex);
	m_textures.erase(found);
}
}
