#include "D3D12Renderer.h"
#include "../MinimalHost.h"
#ifdef RPCS3_HOST_WITH_CORE
#include "core_api.h"
#endif

#include <imgui.h>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace UwpImGuiFrontend::Sample
{
namespace
{
namespace ApplicationModel = winrt::Windows::ApplicationModel;
namespace Core = winrt::Windows::ApplicationModel::Core;
namespace Display = winrt::Windows::Graphics::Display;
namespace GamingInput = winrt::Windows::Gaming::Input;
namespace Storage = winrt::Windows::Storage;
namespace System = winrt::Windows::System;
namespace UI = winrt::Windows::UI::Core;

constexpr ImWchar kFrontendGlyphRanges[] = {
	0x0020, 0x036F,
	0x0E3F, 0x0E3F,
	0x1D00, 0x1EFF,
	0x2000, 0x27FF,
	0x2C60, 0x2C7F,
	0xA700, 0xA7FF,
	0xFB00, 0xFB04,
	0xFE20, 0xFE23,
	0,
};

float PixelScale()
{
	auto display = Display::DisplayInformation::GetForCurrentView();
	double scale = display.RawPixelsPerViewPixel();
	if (!std::isfinite(scale) || scale <= 0.0)
		scale = display.LogicalDpi() / 96.0;
	if (!std::isfinite(scale) || scale <= 0.0)
		scale = 1.0;
	return static_cast<float>(scale);
}

std::pair<std::uint32_t, std::uint32_t> PixelSize(
	const UI::CoreWindow& window)
{
	const auto bounds = window.Bounds();
	const float scale = PixelScale();
	return {
		std::max(1u, static_cast<std::uint32_t>(std::lround(bounds.Width * scale))),
		std::max(1u, static_cast<std::uint32_t>(std::lround(bounds.Height * scale))),
	};
}

void WriteFailure(std::string_view message)
{
	try
	{
		const auto path = std::filesystem::path(
			Storage::ApplicationData::Current().LocalFolder().Path().c_str()) /
			"sample-error.log";
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(message.data(), static_cast<std::streamsize>(message.size()));
	}
	catch (...)
	{
	}
}

std::vector<InputControlDescriptor> SampleGamepadControls()
{
	std::vector<InputControlDescriptor> controls;
	for (const char* button : { "A", "B", "X", "Y", "DPAD-Up", "DPAD-Right",
		"DPAD-Down", "DPAD-Left", "LeftShoulder", "RightShoulder",
		"LeftThumb", "RightThumb", "Menu", "View", "Pointer Press" })
	{
		controls.push_back({ button, button, InputControlKind::Button });
	}
	for (const char* axis : { "LeftStickX+", "LeftStickX-", "LeftStickY+",
		"LeftStickY-", "RightStickX+", "RightStickX-", "RightStickY+",
		"RightStickY-" })
	{
		controls.push_back({ axis, axis, InputControlKind::Axis, 0.0f, 1.0f });
	}
	controls.push_back({ "LeftTrigger", "Left Trigger", InputControlKind::Trigger });
	controls.push_back({ "RightTrigger", "Right Trigger", InputControlKind::Trigger });
	controls.push_back({ "Pointer X", "Pointer X", InputControlKind::Pointer });
	controls.push_back({ "Pointer Y", "Pointer Y", InputControlKind::Pointer });
	return controls;
}

std::vector<InputDeviceDescriptor> EnumerateSampleInputDevices()
{
	const auto gamepads = GamingInput::Gamepad::Gamepads();
	const std::uint32_t count = std::max<std::uint32_t>(1, gamepads.Size());
	std::vector<InputDeviceDescriptor> devices;
	devices.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index)
	{
		devices.push_back({
			.id = "wgi:" + std::to_string(index),
			.label = "Xbox Gamepad " + std::to_string(index + 1),
			.api = "WGI",
			.connected = index < gamepads.Size(),
			.supportsRumble = false,
			.supportsMotion = false,
			.supportsPointer = true,
			.controls = SampleGamepadControls(),
		});
	}
	return devices;
}

std::vector<InputCaptureSample> ReadSampleInputDevices()
{
	std::vector<InputCaptureSample> samples;
	const auto gamepads = GamingInput::Gamepad::Gamepads();
	for (std::uint32_t index = 0; index < gamepads.Size(); ++index)
	{
		const std::string device = "wgi:" + std::to_string(index);
		const auto reading = gamepads.GetAt(index).GetCurrentReading();
		const auto buttons = reading.Buttons;
		const auto held = [buttons](GamingInput::GamepadButtons button) {
			return (static_cast<std::uint32_t>(buttons) &
				static_cast<std::uint32_t>(button)) != 0;
		};
		auto button = [&samples, &device](std::string id, bool pressed) {
			samples.push_back({ device, std::move(id), pressed ? 1.0f : 0.0f,
				InputControlKind::Button });
		};
		auto value = [&samples, &device](std::string id, double input,
			InputControlKind kind = InputControlKind::Axis) {
			samples.push_back({ device, std::move(id),
				static_cast<float>(std::clamp(input, 0.0, 1.0)), kind });
		};
		button("A", held(GamingInput::GamepadButtons::A));
		button("B", held(GamingInput::GamepadButtons::B));
		button("X", held(GamingInput::GamepadButtons::X));
		button("Y", held(GamingInput::GamepadButtons::Y));
		button("DPAD-Up", held(GamingInput::GamepadButtons::DPadUp));
		button("DPAD-Right", held(GamingInput::GamepadButtons::DPadRight));
		button("DPAD-Down", held(GamingInput::GamepadButtons::DPadDown));
		button("DPAD-Left", held(GamingInput::GamepadButtons::DPadLeft));
		button("LeftShoulder", held(GamingInput::GamepadButtons::LeftShoulder));
		button("RightShoulder", held(GamingInput::GamepadButtons::RightShoulder));
		button("LeftThumb", held(GamingInput::GamepadButtons::LeftThumbstick));
		button("RightThumb", held(GamingInput::GamepadButtons::RightThumbstick));
		button("Menu", held(GamingInput::GamepadButtons::Menu));
		button("View", held(GamingInput::GamepadButtons::View));
		button("Pointer Press", held(GamingInput::GamepadButtons::RightThumbstick));
		value("LeftStickX+", reading.LeftThumbstickX);
		value("LeftStickX-", -reading.LeftThumbstickX);
		value("LeftStickY+", reading.LeftThumbstickY);
		value("LeftStickY-", -reading.LeftThumbstickY);
		value("RightStickX+", reading.RightThumbstickX);
		value("RightStickX-", -reading.RightThumbstickX);
		value("RightStickY+", reading.RightThumbstickY);
		value("RightStickY-", -reading.RightThumbstickY);
		value("LeftTrigger", reading.LeftTrigger, InputControlKind::Trigger);
		value("RightTrigger", reading.RightTrigger, InputControlKind::Trigger);
	}
	return samples;
}

class App : public winrt::implements<App, Core::IFrameworkView>
{
public:
	void Initialize(const Core::CoreApplicationView& view)
	{
		view.Activated({ this, &App::OnActivated });
		Core::CoreApplication::Suspending({ this, &App::OnSuspending });
		Core::CoreApplication::Resuming({ this, &App::OnResuming });
	}

	void SetWindow(const UI::CoreWindow& window)
	{
		m_window = window;
		m_display = Display::DisplayInformation::GetForCurrentView();
		m_display.DpiChanged([this](auto&&, auto&&) { ResizeForWindow(); });
		window.SizeChanged([this](auto&&, auto&&) { ResizeForWindow(); });
		window.VisibilityChanged([this](auto&&,
			const UI::VisibilityChangedEventArgs& args) {
			m_visible = args.Visible();
		});
		window.Closed([this](auto&&, auto&&) { m_closed = true; });
		UI::SystemNavigationManager::GetForCurrentView().BackRequested(
			[this](auto&&, const UI::BackRequestedEventArgs& args) {
				args.Handled(true);
				m_systemBackRequested = true;
			});
		window.KeyDown([this](auto&&, const UI::KeyEventArgs& args) {
			m_keys.insert(static_cast<int>(args.VirtualKey()));
		});
		window.KeyUp([this](auto&&, const UI::KeyEventArgs& args) {
			m_keys.erase(static_cast<int>(args.VirtualKey()));
		});
		window.PointerMoved([this](auto&&, const UI::PointerEventArgs& args) {
			UpdatePointer(args);
		});
		window.PointerPressed([this](auto&&, const UI::PointerEventArgs& args) {
			UpdatePointer(args);
		});
		window.PointerReleased([this](auto&&, const UI::PointerEventArgs& args) {
			UpdatePointer(args);
		});
		window.PointerWheelChanged([this](auto&&,
			const UI::PointerEventArgs& args) {
			UpdatePointer(args);
			m_mouseWheel += static_cast<float>(
				args.CurrentPoint().Properties().MouseWheelDelta()) / 120.0f;
		});
		window.PointerExited([this](auto&&, auto&&) {
			m_pointer.reset();
			m_pointerPressed = false;
		});
	}

	// Called by the XAML shell before Run(). The renderer then creates a
	// composition swap chain and attaches it through ISwapChainPanelNative.
	void SetSwapChainPanel(IUnknown* panel, std::uint32_t pixelWidth,
		std::uint32_t pixelHeight, float rasterizationScale)
	{
		m_swapChainPanel = panel;
		m_xamlHosted = panel != nullptr;
		m_pixelWidth = std::max(1u, pixelWidth);
		m_pixelHeight = std::max(1u, pixelHeight);
		m_rasterizationScale = std::isfinite(rasterizationScale) && rasterizationScale > 0.0f
			? rasterizationScale : 1.0f;
	}

	void RequestClose() noexcept { m_closed = true; }

	// SwapChainPanel::SetSwapChain must be performed on the XAML UI thread.
	// The shell calls this during rpcs3_frontend_create(), before Run() is
	// dispatched to the background render loop.
	void InitializeXamlGraphics()
	{
		if (m_graphicsInitialized)
			return;

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		const auto width = m_pixelWidth;
		const auto height = m_pixelHeight;
		const float uiScale = std::max(0.1f, std::min(
			static_cast<float>(width) / 1280.0f,
			static_cast<float>(height) / 720.0f));
		LoadFrontendFonts(io, uiScale);
		m_renderer.Initialize(m_swapChainPanel.Get(), width, height);
		m_frontend.Host().SetTextureCallbacks(
			[this](const std::filesystem::path& path) { return m_renderer.LoadTexture(path); },
			[this](TextureHandle texture) { m_renderer.ReleaseTexture(texture); });
		m_graphicsInitialized = true;
	}

	void Load(const winrt::hstring&)
	{
	}

	void Run()
	{
		try
		{
			if (!m_graphicsInitialized)
			{
				IMGUI_CHECKVERSION();
				ImGui::CreateContext();
				ImGuiIO& io = ImGui::GetIO();
				io.IniFilename = nullptr;
				io.LogFilename = nullptr;
				const auto [width, height] = PixelSize(m_window);
				const float uiScale = std::max(0.1f, std::min(
					static_cast<float>(width) / 1280.0f,
					static_cast<float>(height) / 720.0f));
				LoadFrontendFonts(io, uiScale);
				m_renderer.Initialize(m_window, width, height);
				m_frontend.Host().SetTextureCallbacks(
					[this](const std::filesystem::path& path) { return m_renderer.LoadTexture(path); },
					[this](TextureHandle texture) { m_renderer.ReleaseTexture(texture); });
				m_graphicsInitialized = true;
			}
			m_frontend.Host().SetInputCallbacks(EnumerateSampleInputDevices,
				ReadSampleInputDevices);
			m_frontend.Host().SetDsuEndpoint({});
			m_frontend.Initialize();
#ifdef RPCS3_HOST_WITH_CORE
			m_coreDispatcher = m_window.Dispatcher();
			rpcs3_core_callbacks callbacks{sizeof(callbacks), 2, this,
				[](void* user, uint32_t level, uint64_t, const char*, const char* text)
				{
					auto& app = *static_cast<App*>(user);
					// The core persists every log independently. Keep a bounded UI tail.
					if (app.m_coreLogTail.size() == 256) app.m_coreLogTail.erase(app.m_coreLogTail.begin());
					app.m_coreLogTail.push_back(std::to_string(level) + ": " + text);
				},
				[](void* user, uint32_t type, uint32_t command, int32_t result, const char* text)
				{
					auto& app = *static_cast<App*>(user);
					app.m_frontend.Host().OnCoreEvent(type, command, result, text);
					if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && command == RPCS3_CORE_COMMAND_INITIALIZE)
						app.m_coreReady = result == RPCS3_CORE_OK;
					if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && command == RPCS3_CORE_COMMAND_SHUTDOWN)
					{
						app.m_coreShutdownComplete = true;
						app.m_coreShutdownResult = result;
					}
				},
				[](void* user)
				{
					const auto& app = *static_cast<App*>(user);
					app.m_coreDispatcher.RunAsync(UI::CoreDispatcherPriority::Normal, [] {});
				}};
			if (rpcs3_core_set_callbacks(&callbacks) != RPCS3_CORE_OK)
				throw std::runtime_error("RPCS3 callback registration failed");
			const auto stateRoot = (m_frontend.Host().StateRoot() / "rpcs3").u8string();
			std::filesystem::create_directories(m_frontend.Host().StateRoot() / "rpcs3");
			if (rpcs3_core_initialize(reinterpret_cast<const char*>(stateRoot.c_str())) != RPCS3_CORE_OK)
				throw std::runtime_error("RPCS3 core initialization failed");
#endif
			m_initialized = true;
			m_lastFrame = std::chrono::steady_clock::now();

			auto dispatcher = m_window.Dispatcher();
			while (!m_closed
#ifdef RPCS3_HOST_WITH_CORE
				|| !m_coreShutdownComplete || !m_coreReleased
#endif
			)
			{
#ifdef RPCS3_HOST_WITH_CORE
				(void)rpcs3_core_pump();
				if (m_closed)
				{
					if (!m_coreShutdownRequested)
					{
						const auto result = rpcs3_core_shutdown();
						m_coreShutdownRequested = result == RPCS3_CORE_OK;
						if (result != RPCS3_CORE_OK) WriteFailure("RPCS3 shutdown request rejected");
					}
					if (m_coreShutdownComplete)
					{
						const auto result = rpcs3_core_release();
						m_coreReleased = result == RPCS3_CORE_OK;
						if (result == RPCS3_CORE_FAULTED)
							WriteFailure("RPCS3 faulted: shutdown/unload is unsafe");
					}
					if (!m_coreReleased)
					{
						if (m_xamlHosted)
							std::this_thread::yield();
						else
							dispatcher.ProcessEvents(UI::CoreProcessEventsOption::ProcessOneAndAllPending);
					}
					continue;
				}
#endif
				if (!m_visible)
				{
					if (m_xamlHosted)
						Sleep(16);
					else
						dispatcher.ProcessEvents(
							UI::CoreProcessEventsOption::ProcessOneAndAllPending);
					continue;
				}
				if (!m_xamlHosted)
					dispatcher.ProcessEvents(
						UI::CoreProcessEventsOption::ProcessAllIfPresent);
				DrawFrame();
			}
		}
		catch (const winrt::hresult_error& error)
		{
			WriteFailure(winrt::to_string(error.message()));
		}
		catch (const std::exception& error)
		{
			WriteFailure(error.what());
		}
		Shutdown();
	}

	void Uninitialize()
	{
		Shutdown();
	}

private:
	void LoadFrontendFonts(ImGuiIO& io, float uiScale)
	{
		const std::filesystem::path path = m_frontend.Host().ResourceRoot() /
			"fonts" / "Lato-Medium.ttf";
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input)
			throw std::runtime_error("The packaged Lato Medium UI font is missing");
		const std::streamsize size = input.tellg();
		if (size <= 0)
			throw std::runtime_error("The packaged Lato Medium UI font is empty");
		m_fontData.resize(static_cast<std::size_t>(size));
		input.seekg(0, std::ios::beg);
		if (!input.read(reinterpret_cast<char*>(m_fontData.data()), size))
			throw std::runtime_error("The packaged Lato Medium UI font could not be read");

		ImFontConfig configuration;
		configuration.FontDataOwnedByAtlas = false;
		configuration.OversampleH = 2;
		configuration.OversampleV = 2;
		m_textFont = io.Fonts->AddFontFromMemoryTTF(m_fontData.data(),
			static_cast<int>(m_fontData.size()),
			std::max(10.0f, std::round(18.0f * uiScale)),
			&configuration, kFrontendGlyphRanges);
		m_shellFont = io.Fonts->AddFontFromMemoryTTF(m_fontData.data(),
			static_cast<int>(m_fontData.size()),
			std::max(10.0f, std::round(24.0f * uiScale)),
			&configuration, kFrontendGlyphRanges);
		if (!m_textFont || !m_shellFont)
			throw std::runtime_error("The packaged Lato Medium UI font could not be loaded");
		io.FontDefault = m_textFont;
	}

	bool KeyDown(System::VirtualKey key) const
	{
		return m_keys.find(static_cast<int>(key)) != m_keys.end();
	}

	void ResizeForWindow()
	{
		if (!m_window)
			return;
		const auto [width, height] = PixelSize(m_window);
		if (m_initialized)
			m_renderer.Resize(width, height);
	}

	void UpdatePointer(const UI::PointerEventArgs& args)
	{
		const auto point = args.CurrentPoint();
		const float scale = PixelScale();
		m_pointer = Vec2{ point.Position().X * scale, point.Position().Y * scale };
		m_pointerPressed = point.Properties().IsLeftButtonPressed();
	}

	FrameInput ReadInput(float deltaSeconds)
	{
		FrameInput input;
		input.deltaSeconds = deltaSeconds;
		input.up = KeyDown(System::VirtualKey::Up) || KeyDown(System::VirtualKey::W);
		input.down = KeyDown(System::VirtualKey::Down) || KeyDown(System::VirtualKey::S);
		input.left = KeyDown(System::VirtualKey::Left) || KeyDown(System::VirtualKey::A);
		input.right = KeyDown(System::VirtualKey::Right) || KeyDown(System::VirtualKey::D);
		input.accept = KeyDown(System::VirtualKey::Enter) || KeyDown(System::VirtualKey::Space);
		input.back = std::exchange(m_systemBackRequested, false) ||
			KeyDown(System::VirtualKey::Escape) || KeyDown(System::VirtualKey::Back);
		input.context = KeyDown(System::VirtualKey::X);
		input.alternate = KeyDown(System::VirtualKey::Y);
		input.menu = KeyDown(System::VirtualKey::F1);
		input.view = KeyDown(System::VirtualKey::Tab);
		input.pointer = m_pointer;
		input.pointerPressed = m_pointerPressed;

		const auto gamepads = GamingInput::Gamepad::Gamepads();
		if (gamepads.Size() > 0)
		{
			const auto reading = gamepads.GetAt(0).GetCurrentReading();
			const auto buttons = reading.Buttons;
			const auto held = [buttons](GamingInput::GamepadButtons button) {
				return (static_cast<std::uint32_t>(buttons) &
					static_cast<std::uint32_t>(button)) != 0;
			};
			input.up |= held(GamingInput::GamepadButtons::DPadUp);
			input.down |= held(GamingInput::GamepadButtons::DPadDown);
			input.left |= held(GamingInput::GamepadButtons::DPadLeft);
			input.right |= held(GamingInput::GamepadButtons::DPadRight);
			input.accept |= held(GamingInput::GamepadButtons::A);
			input.back |= held(GamingInput::GamepadButtons::B);
			input.context |= held(GamingInput::GamepadButtons::X);
			input.alternate |= held(GamingInput::GamepadButtons::Y);
			input.menu |= held(GamingInput::GamepadButtons::Menu);
			input.view |= held(GamingInput::GamepadButtons::View);
			input.leftShoulder = held(GamingInput::GamepadButtons::LeftShoulder);
			input.rightShoulder = held(GamingInput::GamepadButtons::RightShoulder);
			input.leftStickX = static_cast<float>(reading.LeftThumbstickX);
			input.leftStickY = static_cast<float>(reading.LeftThumbstickY);
			input.rightStickX = static_cast<float>(reading.RightThumbstickX);
			input.rightStickY = static_cast<float>(reading.RightThumbstickY);
			input.controllerInputHeld = static_cast<std::uint32_t>(buttons) != 0 ||
				std::abs(input.leftStickX) > 0.2f ||
				std::abs(input.leftStickY) > 0.2f ||
				std::abs(input.rightStickX) > 0.2f ||
				std::abs(input.rightStickY) > 0.2f ||
				reading.LeftTrigger > 0.2 || reading.RightTrigger > 0.2;
		}
		return input;
	}

	void DrawFrame()
	{
#ifdef RPCS3_HOST_WITH_CORE
		(void)rpcs3_core_pump();
		if (m_coreReady && !m_coreGraphicsChecked)
		{
			const auto result = rpcs3_core_attach_d3d12(m_renderer.Device(), m_renderer.Queue());
			if (result == RPCS3_CORE_OK || result == RPCS3_CORE_UNSUPPORTED_RENDERER)
				m_coreGraphicsChecked = true;
			else if (result != RPCS3_CORE_BUSY)
				throw std::runtime_error("RPCS3 D3D12 attachment failed");
		}
#endif
		const auto now = std::chrono::steady_clock::now();
		const float deltaSeconds = std::clamp(
			std::chrono::duration<float>(now - m_lastFrame).count(),
			1.0f / 1000.0f, 0.1f);
		m_lastFrame = now;
		FrameInput input = ReadInput(deltaSeconds);
		SubmitImGuiNavigationInput(input,
			GamingInput::Gamepad::Gamepads().Size() > 0);

		ImGuiIO& io = ImGui::GetIO();
		if (m_pointer)
			io.MousePos = { m_pointer->x, m_pointer->y };
		else
			io.MousePos = { -FLT_MAX, -FLT_MAX };
		io.MouseDown[0] = m_pointerPressed;
		io.MouseWheel = std::exchange(m_mouseWheel, 0.0f);

		m_renderer.BeginFrame(deltaSeconds);
		const InterfaceSettingsModel& settings =
			m_frontend.Host().InterfaceSettings();
		ShellPresentation presentation;
		presentation.themeIndex = settings.themeIndex;
		presentation.showActionHints = settings.showActionHints;
		presentation.font = m_shellFont;
		presentation.controllerStatus =
			GamingInput::Gamepad::Gamepads().Size() > 0 ? "1 pad" : "No pad";
		m_frontend.DrawFrame(input, presentation);
#ifdef RPCS3_HOST_WITH_CORE
		void* frameResource = nullptr;
		uint64_t serial = 0;
		if (rpcs3_core_acquire_d3d12_frame(&frameResource, &serial) == RPCS3_CORE_OK)
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> frame;
			frame.Attach(static_cast<ID3D12Resource*>(frameResource));
			const auto texture = m_renderer.ImportCoreFrame(frame.Get());
			ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.8f, io.DisplaySize.y * 0.8f), ImGuiCond_FirstUseEver);
			if (ImGui::Begin("RPCS3 video"))
			{
				const auto available = ImGui::GetContentRegionAvail();
				const float scale = std::min(available.x / texture.width, available.y / texture.height);
				if (scale > 0) ImGui::Image(reinterpret_cast<ImTextureID>(texture.id), ImVec2(texture.width * scale, texture.height * scale));
			}
			ImGui::End();
		}
		else if (m_frontend.Host().GetRunningContent().running)
		{
			ImGui::SetNextWindowSize(ImVec2(480.0f, 150.0f), ImGuiCond_FirstUseEver);
			if (ImGui::Begin("RPCS3 video"))
			{
				ImGui::TextUnformatted("The core is active, but no video frame is available yet.");
				ImGui::TextWrapped("This does not confirm that the game is progressing. Check RPCS3.log for guest or renderer errors.");
				if (ImGui::Button("Stop game"))
					m_frontend.Host().Execute(HostCommand::StopContent);
			}
			ImGui::End();
		}
#endif
		m_renderer.EndFrame();
	}

	void Shutdown()
	{
		if (m_initialized)
		{
#ifdef RPCS3_HOST_WITH_CORE
			if (!m_coreReleased && !m_coreShutdownRequested)
				m_coreShutdownRequested = rpcs3_core_shutdown() == RPCS3_CORE_OK;
#endif
			m_frontend.Shutdown();
			m_initialized = false;
		}
		m_renderer.Shutdown();
		if (ImGui::GetCurrentContext())
			ImGui::DestroyContext();
	}

	void OnActivated(const Core::CoreApplicationView&,
		const ApplicationModel::Activation::IActivatedEventArgs&)
	{
		m_window.Activate();
	}

	void OnSuspending(const winrt::Windows::Foundation::IInspectable&,
		const ApplicationModel::SuspendingEventArgs& args)
	{
		const auto deferral = args.SuspendingOperation().GetDeferral();
		m_renderer.WaitForGpu();
		deferral.Complete();
	}

	void OnResuming(const winrt::Windows::Foundation::IInspectable&,
		const winrt::Windows::Foundation::IInspectable&)
	{
		m_lastFrame = std::chrono::steady_clock::now();
		ResizeForWindow();
	}

	UI::CoreWindow m_window{ nullptr };
	Microsoft::WRL::ComPtr<IUnknown> m_swapChainPanel;
	Display::DisplayInformation m_display{ nullptr };
	D3D12Renderer m_renderer;
	MinimalFrontend m_frontend;
	std::vector<unsigned char> m_fontData;
	ImFont* m_textFont = nullptr;
	ImFont* m_shellFont = nullptr;
	std::unordered_set<int> m_keys;
	std::optional<Vec2> m_pointer;
	std::chrono::steady_clock::time_point m_lastFrame{};
	float m_mouseWheel = 0.0f;
	bool m_pointerPressed = false;
	bool m_systemBackRequested = false;
	bool m_visible = true;
	bool m_closed = false;
	bool m_initialized = false;
	bool m_graphicsInitialized = false;
	bool m_xamlHosted = false;
	std::uint32_t m_pixelWidth = 1;
	std::uint32_t m_pixelHeight = 1;
	float m_rasterizationScale = 1.0f;
#ifdef RPCS3_HOST_WITH_CORE
	std::vector<std::string> m_coreLogTail;
	UI::CoreDispatcher m_coreDispatcher{nullptr};
	bool m_coreShutdownRequested = false;
	bool m_coreReady = false;
	bool m_coreGraphicsChecked = false;
	bool m_coreShutdownComplete = false;
	bool m_coreReleased = false;
	int32_t m_coreShutdownResult = RPCS3_CORE_OK;
#endif
};

struct FrontendRuntimeHandle
{
	winrt::com_ptr<App> app;
};

extern "C" __declspec(dllexport) void* rpcs3_frontend_create(IUnknown* swapChainPanel,
	std::uint32_t pixelWidth, std::uint32_t pixelHeight, float rasterizationScale)
{
	try
	{
		if (!swapChainPanel)
			throw std::invalid_argument("XAML SwapChainPanel is null");
		auto handle = std::make_unique<FrontendRuntimeHandle>();
		handle->app = winrt::make_self<App>();
		handle->app->SetWindow(UI::CoreWindow::GetForCurrentThread());
		handle->app->SetSwapChainPanel(swapChainPanel, pixelWidth, pixelHeight,
			rasterizationScale);
		handle->app->InitializeXamlGraphics();
		return handle.release();
	}
	catch (const winrt::hresult_error& error)
	{
		WriteFailure("XAML frontend creation: " + winrt::to_string(error.message()));
	}
	catch (const std::exception& error)
	{
		WriteFailure("XAML frontend creation: " + std::string(error.what()));
	}
	catch (...)
	{
		WriteFailure("XAML frontend creation: unknown exception");
	}
	return nullptr;
}

extern "C" __declspec(dllexport) void rpcs3_frontend_run(void* value)
{
	try
	{
		if (auto* handle = static_cast<FrontendRuntimeHandle*>(value))
			handle->app->Run();
	}
	catch (const std::exception& error)
	{
		WriteFailure("XAML frontend worker: " + std::string(error.what()));
	}
	catch (...)
	{
		WriteFailure("XAML frontend worker: unknown exception");
	}
}

extern "C" __declspec(dllexport) void rpcs3_frontend_stop(void* value)
{
	if (auto* handle = static_cast<FrontendRuntimeHandle*>(value))
		handle->app->RequestClose();
}

extern "C" __declspec(dllexport) void rpcs3_frontend_destroy(void* value)
{
	delete static_cast<FrontendRuntimeHandle*>(value);
}

class AppSource :
	public winrt::implements<AppSource, Core::IFrameworkViewSource>
{
public:
	Core::IFrameworkView CreateView()
	{
		return winrt::make<App>();
	}
};
}
}

#ifndef RPCS3_XAML_HOST
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	try
	{
		try
		{
			winrt::init_apartment(winrt::apartment_type::single_threaded);
		}
		catch (const winrt::hresult_error& error)
		{
			if (static_cast<HRESULT>(error.code()) != RPC_E_CHANGED_MODE)
				throw;
		}
		winrt::Windows::ApplicationModel::Core::CoreApplication::Run(
			winrt::make<UwpImGuiFrontend::Sample::AppSource>());
	}
	catch (const winrt::hresult_error& error)
	{
		UwpImGuiFrontend::Sample::WriteFailure(winrt::to_string(error.message()));
	}
	catch (const std::exception& error)
	{
		UwpImGuiFrontend::Sample::WriteFailure(error.what());
	}
	return 0;
}
#endif
