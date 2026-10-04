#include "stdafx.h"
#include "core_api.h"
#include "host_runtime.h"
#include "brokered_files.h"
#include "Emu/System.h"
#include "Emu/emu_callbacks.h"
#include "Emu/system_config.h"
#include "Emu/vfs_config.h"
#include "Emu/IPC_config.h"
#include "Emu/system_utils.hpp"
#include "Emu/IdManager.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/D3D12/D3D12Presentation.h"
#ifdef RPCS3_UWP_MESA
#include "Emu/RSX/GL/GLGSRender.h"
#include "mesa_frame.h"
#endif
#ifdef RPCS3_UWP_D3D12
#include "Emu/RSX/D3D12/D3D12GSRender.h"
#endif
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Audio/XAudio2/XAudio2Backend.h"
#include "Emu/Audio/audio_device_enumerator.h"
#include "audio_source.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Input/pad_thread.h"
#include "Emu/Io/pad_config.h"
#include "Emu/Io/mouse_config.h"
#include "util/video_source.h"
#include "rpcs3_version.h"
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <chrono>

std::string g_input_config_override;
atomic_t<bool> g_headless{false};
cfg_input_configurations g_cfg_input_configs;
mouse_config g_cfg_mouse;
bool rpcs3_embedded_install_exception_handler();
bool rpcs3_embedded_remove_exception_handler();

namespace
{
	std::mutex s_api_mutex;
	std::shared_ptr<rpcs3::embedded::host_runtime> s_runtime;
	std::atomic<bool> s_initialized{false}, s_init_requested{false}, s_closing{false}, s_faulted{false};
	std::atomic<uint32_t> s_state{0};
	bool s_listener_added = false;
	struct config_snapshot_entry
	{
		uint32_t type = RPCS3_CORE_CONFIG_TEXT;
		uint32_t flags = 0;
		std::string path, name, value, default_value, enum_values;
		std::string group, minimum_value, maximum_value, restriction;
	};
	std::mutex s_config_mutex;
	std::vector<config_snapshot_entry> s_config_snapshot;

	std::string setting_restriction(std::string_view path)
	{
		if (path == "Video/Renderer")
		{
#ifndef RPCS3_UWP_MESA
			return "Renderer is selected by the shared D3D12 host";
#endif
		}
		if (path == "Audio/Renderer") return "The UWP host uses XAudio2 on the system default output device";
		if (path == "Audio/Audio Device") return "XAudio2 follows the system default output device in UWP";
		if (path == "Audio/Audio Provider") return "The core selects the audio provider for the loaded executable";
		if (path == "Core/PPU Decoder" || path == "Core/SPU Decoder")
			return "The UWP port uses interpreters; generated-code exception recovery is unavailable";
		if (path == "Input/Output/Keyboard" || path == "Input/Output/Mouse" ||
			path == "Input/Output/Camera" || path == "Audio/Music Handler" ||
			path == "Audio/Microphone Type") return "Only the Null handler is integrated for this device";
		if (path == "Net/UPNP Enabled") return "UPnP is excluded from the UWP build";
		if (path == "Net/Derive MAC from PSID") return "The UWP core derives the MAC address from the emulated PSID";
		if (path.starts_with("Video/Vulkan/")) return "Vulkan is excluded from the UWP build";
		if (path.starts_with("Mounts/")) return "Storage locations require a brokered picker grant";
		if (path.starts_with("IPC/")) return "Desktop IPC is not integrated in this host";
		return {};
	}

	bool is_uwp_locked_setting(std::string_view path)
	{
		return !setting_restriction(path).empty();
	}

	void normalize_host_settings()
	{
#ifdef RPCS3_UWP_MESA
		if (g_cfg.video.renderer != video_renderer::opengl)
#endif
			g_cfg.video.renderer.set(video_renderer::null);
		g_cfg.audio.renderer.set(audio_renderer::xaudio);
		g_cfg.audio.audio_device.set(std::string(audio_device_enumerator::DEFAULT_DEV_ID));
		g_cfg.io.keyboard.set(keyboard_handler::null);
		g_cfg.io.mouse.set(mouse_handler::null);
		g_cfg.io.camera.set(camera_handler::null);
		g_cfg.audio.music.set(music_handler::null);
		g_cfg.audio.microphone_type.set(microphone_handler::null);
		g_cfg.net.upnp_enabled.set(false);
		g_cfg.net.derive_mac_from_psid.set(true);
		g_cfg.core.ppu_decoder.set(ppu_decoder_type::_static);
		g_cfg.core.spu_decoder.set(spu_decoder_type::_static);
	}

	void collect_config(const cfg::node& owner, std::string_view prefix,
		std::vector<config_snapshot_entry>& output, std::string_view group = {},
		const cfg::node* defaults = nullptr)
	{
		for (const cfg::_base* item : owner.get_nodes())
		{
			const std::string path = prefix.empty() ? item->get_name() :
				std::string(prefix) + "/" + item->get_name();
			if (item->get_type() == cfg::type::node)
			{
				const cfg::node* childDefaults = nullptr;
				if (defaults) for (const auto* child : defaults->get_nodes())
					if (child->get_name() == item->get_name()) childDefaults = static_cast<const cfg::node*>(child);
				collect_config(static_cast<const cfg::node&>(*item), path, output,
					group.empty() ? item->get_name() : group, childDefaults);
				continue;
			}
			config_snapshot_entry entry;
			entry.path = path;
			entry.name = item->get_name();
			entry.value = item->to_string();
			entry.default_value = item->def_to_string();
			entry.group = group.empty() ? item->get_name() : std::string(group);
			entry.restriction = setting_restriction(path);
			entry.flags = (item->get_is_dynamic() ? RPCS3_CORE_CONFIG_DYNAMIC : 0) |
				(is_uwp_locked_setting(path) ? RPCS3_CORE_CONFIG_READ_ONLY : 0);
			switch (item->get_type())
			{
			case cfg::type::_bool: entry.type = RPCS3_CORE_CONFIG_BOOL; break;
			case cfg::type::_enum:
			{
				entry.type = RPCS3_CORE_CONFIG_ENUM;
				const auto choices = item->to_list();
				for (usz index = 0; index < choices.size(); ++index)
				{
					if (index) entry.enum_values.push_back('\x1f');
					entry.enum_values += choices[index];
				}
				break;
			}
			case cfg::type::_int:
			case cfg::type::uint:
			case cfg::type::uint128: entry.type = RPCS3_CORE_CONFIG_NUMBER; break;
			case cfg::type::string: entry.type = RPCS3_CORE_CONFIG_TEXT; break;
			default:
				entry.type = RPCS3_CORE_CONFIG_COLLECTION;
				entry.value = item->to_yaml();
				if (defaults) for (const auto* child : defaults->get_nodes())
					if (child->get_name() == item->get_name()) entry.default_value = child->to_yaml();
				break;
			}
			if (entry.type == RPCS3_CORE_CONFIG_NUMBER || item->get_type() == cfg::type::string)
			{
				const auto range = item->to_list();
				if (range.size() == 2 && item->get_type() != cfg::type::uint128)
				{
					entry.type = RPCS3_CORE_CONFIG_NUMBER;
					entry.minimum_value = range[0]; entry.maximum_value = range[1];
				}
			}
			if (is_uwp_locked_setting(path))
			{
				entry.default_value = entry.value;
				if (entry.type == RPCS3_CORE_CONFIG_ENUM) entry.enum_values = entry.value;
			}
#ifdef RPCS3_UWP_MESA
			if (path == "Video/Renderer")
			{
				entry.value = g_cfg.video.renderer == video_renderer::opengl ? "OpenGL (Mesa Gallium D3D12)" : "Direct3D 12";
				entry.default_value = "Direct3D 12";
				entry.enum_values = "Direct3D 12\x1fOpenGL (Mesa Gallium D3D12)";
			}
#endif
			output.push_back(std::move(entry));
		}
	}

	void refresh_config_snapshot()
	{
		std::vector<config_snapshot_entry> snapshot;
		cfg_root defaults;
		collect_config(g_cfg, {}, snapshot, {}, &defaults);
		cfg_vfs mountDefaults;
		collect_config(g_cfg_vfs, "Mounts", snapshot, "Mounts", &mountDefaults);
		cfg_ipc ipcDefaults;
		collect_config(g_cfg_ipc, "IPC", snapshot, "IPC", &ipcDefaults);
		std::lock_guard lock(s_config_mutex);
		s_config_snapshot = std::move(snapshot);
	}

	cfg::_base* find_config(std::string_view path)
	{
		const auto search = [](auto&& self, cfg::node& node, std::string_view prefix,
			std::string_view wanted) -> cfg::_base*
		{
			for (cfg::_base* item : node.get_nodes())
			{
				const std::string full = prefix.empty() ? item->get_name() : std::string(prefix) + "/" + item->get_name();
				if (full == wanted) return item;
				if (item->get_type() == cfg::type::node)
					if (auto* found = self(self, static_cast<cfg::node&>(*item), full, wanted)) return found;
			}
			return nullptr;
		};
		if (path == "Mounts") return &g_cfg_vfs;
		if (path == "IPC") return &g_cfg_ipc;
		if (auto* found = search(search, g_cfg, {}, path)) return found;
		if (auto* found = search(search, g_cfg_vfs, "Mounts", path)) return found;
		return search(search, g_cfg_ipc, "IPC", path);
	}

	std::shared_ptr<rpcs3::embedded::host_runtime> runtime()
	{
		std::lock_guard lock(s_api_mutex);
		return s_runtime;
	}

	bool save_current_config()
	{
		const auto title = Emu.GetTitleID();
		const std::string path = title.empty() ? fs::get_config_dir(true) + "config.yml" :
			rpcs3::utils::get_custom_config_path(title);
		const auto data = g_cfg.to_string();
		if (fs::create_path(fs::get_parent_dir(path)))
		{
			fs::pending_file file(path);
			if (file.file && file.file.write(data.data(), data.size()) == data.size() && file.commit())
			{
				g_backup_cfg.from_string(data);
				return true;
			}
		}
		if (auto host = runtime()) host->error("Unable to save configuration: " + path);
		return false;
	}

	struct host_log_listener final : logs::listener
	{
		void log(u64 stamp, const logs::message& msg, std::string_view prefix, std::string_view text) override
		{
			if (auto host = runtime())
				host->log(static_cast<uint32_t>(static_cast<logs::level>(msg)), stamp,
					msg->name, std::string(prefix) + (prefix.empty() ? "" : ": ") + std::string(text));
		}
	} s_listener;

	void state_changed()
	{
		s_state = static_cast<uint32_t>(Emu.GetStatus(false));
		if (auto host = runtime()) host->event(RPCS3_CORE_EVENT_STATE_CHANGED, 0, static_cast<int32_t>(s_state.load()));
	}

	int32_t queue_command(uint32_t command, std::function<int32_t()> function)
	{
		auto host = runtime();
		if (!host || !s_init_requested) return RPCS3_CORE_NOT_INITIALIZED;
		if (s_closing) return RPCS3_CORE_BUSY;
		if (s_faulted && command != RPCS3_CORE_COMMAND_STOP && command != RPCS3_CORE_COMMAND_SHUTDOWN)
			return RPCS3_CORE_FAULTED;
		return host->submit(command, [function = std::move(function), command, host]() -> int32_t
		{
			if (!s_initialized) return RPCS3_CORE_NOT_INITIALIZED;
			if (s_faulted && command != RPCS3_CORE_COMMAND_STOP && command != RPCS3_CORE_COMMAND_SHUTDOWN)
				return RPCS3_CORE_FAULTED;
			try
			{
				const auto result = function();
				if (result == RPCS3_CORE_OK && (command == RPCS3_CORE_COMMAND_BOOT ||
					command == RPCS3_CORE_COMMAND_STOP || command == RPCS3_CORE_COMMAND_INSTALL_FIRMWARE))
					refresh_config_snapshot();
				state_changed();
				return result;
			}
			catch (const std::exception& error)
			{
				s_faulted = true;
				host->error(error.what());
				state_changed();
				return RPCS3_CORE_INTERNAL_ERROR;
			}
			catch (...)
			{
				s_faulted = true;
				host->error("Embedded command failed; stop the core before retrying");
				state_changed();
				return RPCS3_CORE_INTERNAL_ERROR;
			}
		});
	}

	void install_callbacks()
	{
		auto& cb = g_emu_callbacks;
		cb.call_from_main_thread = [](std::function<void()> function, atomic_t<u32>* wake)
		{
			auto task = [function = std::move(function), wake]()
			{
				try { function(); }
				catch (...)
				{
					s_faulted = true;
					if (wake) { *wake = true; wake->notify_one(); }
					throw;
				}
				if (wake) { *wake = true; wake->notify_one(); }
			};
			auto host = runtime();
			if (!host) throw std::runtime_error("Control runtime unavailable");
			if (host->is_owner()) { task(); return; }
			if (host->submit(0, [task = std::move(task)]() { task(); return RPCS3_CORE_OK; }) != RPCS3_CORE_OK)
			{
				if (wake) { *wake = true; wake->notify_one(); }
				throw std::runtime_error("Control queue is full");
			}
		};
		cb.on_run = [](bool) { state_changed(); };
		cb.on_pause = cb.on_resume = cb.on_stop = cb.on_ready = state_changed;
		cb.on_missing_fw = [] { if (auto host = runtime()) host->error("PS3 firmware is missing"); };
		cb.enable_disc_eject = cb.enable_disc_insert = [](bool) {};
		cb.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>> closed, int)
		{
			if (!closed || !*closed)
			{
				s_faulted = true;
				if (auto host = runtime()) host->error("Emulation stop timed out; DLL must remain loaded");
			}
		};
		cb.on_save_state_progress = [](auto, auto, auto, auto) {};
		cb.try_to_quit = [](bool, std::function<void()>)
		{
			if (auto host = runtime()) host->event(RPCS3_CORE_EVENT_QUIT_REQUESTED, 0, RPCS3_CORE_OK);
			// The host decides whether to stop/release; do not tear it down here.
			return false;
		};
		cb.handle_taskbar_progress = [](s32, s32) {};
		cb.init_kb_handler = [] { g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager()); };
		cb.init_mouse_handler = [] { g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager()); };
		cb.init_pad_handler = [](std::string_view title) { g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, title); };
		cb.init_gs_render = [](utils::serial* ar)
		{
			normalize_host_settings();
#ifdef RPCS3_UWP_MESA
			if (g_cfg.video.renderer == video_renderer::opengl)
			{
				(void)d3d12::presentation().device_and_queue();
				g_fxo->init<rsx::thread, named_thread<GLGSRender>>(ar);
				if (auto host = runtime()) host->log(6, 0, "Embedding", "RSX: OpenGL / Mesa Gallium D3D12 renderer initialized");
				return;
			}
#endif
			ensure(g_cfg.video.renderer == video_renderer::null);
#ifdef RPCS3_UWP_D3D12
			try
			{
				(void)d3d12::presentation().device_and_queue();
			}
			catch (const std::logic_error&)
			{
				throw std::runtime_error("D3D12 device is not attached; refusing a silent Null renderer fallback");
			}
			g_fxo->init<rsx::thread, named_thread<D3D12GSRender>>(ar);
			if (auto host = runtime()) host->log(6, 0, "Embedding", "RSX: shared D3D12 renderer initialized");
#else
			g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
#endif
		};
		cb.get_gs_frame = []() -> std::unique_ptr<GSFrameBase>
		{
#ifdef RPCS3_UWP_MESA
			if (g_cfg.video.renderer == video_renderer::opengl) return make_mesa_frame();
#endif
			return {};
		};
		cb.close_gs_frame = [] {};
		cb.get_audio = []() -> std::shared_ptr<AudioBackend>
		{
			normalize_host_settings();
			return std::make_shared<XAudio2Backend>();
		};
		cb.get_audio_enumerator = [](u64 renderer) -> std::shared_ptr<audio_device_enumerator>
		{
			if (renderer != static_cast<u64>(audio_renderer::xaudio))
				throw std::runtime_error("UWP audio enumeration is not integrated");
			class default_output final : public audio_device_enumerator
			{
			public:
				std::vector<audio_device> get_output_devices() override
				{
					return {{std::string(DEFAULT_DEV_ID), "System default (XAudio2)", 8}};
				}
			};
			return std::make_shared<default_output>();
		};
		cb.get_camera_handler = [] { return std::make_shared<null_camera_handler>(); };
		cb.get_music_handler = [] { return std::make_shared<null_music_handler>(); };
		cb.get_msg_dialog = [] { return std::shared_ptr<MsgDialogBase>{}; };
		cb.get_osk_dialog = [] { return std::shared_ptr<OskDialogBase>{}; };
		cb.get_save_dialog = [] { return std::unique_ptr<SaveDialogBase>{}; };
		cb.get_trophy_notification_dialog = [] { return std::unique_ptr<TrophyNotificationBase>{}; };
		cb.update_emu_settings = [] {};
		cb.save_emu_settings = [] { Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID()); };
		cb.get_localized_string = [](localized_string_id, const char* args) { return std::string(args ? args : ""); };
		cb.get_localized_u32string = [](localized_string_id, const char*) { return std::u32string{}; };
		cb.get_localized_setting = [](const cfg::_base*, u32) { return std::string{}; };
		cb.get_photo_path = [](std::string_view) { return std::string{}; };
		cb.play_sound = [](const std::string&, std::optional<f32>) {};
		cb.get_image_info = [](const std::string&, std::string&, s32&, s32&, s32&) { return false; };
		cb.get_scaled_image = [](const std::string&, s32, s32, s32&, s32&, u8*, bool) { return false; };
		cb.get_font_dirs = [] { return std::vector<std::string>{fs::get_executable_dir() + "/fonts/"}; };
		cb.on_install_pkgs = [](const std::vector<std::string>&, bool) { return false; };
		cb.add_breakpoint = [](u32) {};
		cb.display_sleep_control_supported = [] { return false; };
		cb.enable_display_sleep = [](bool) {};
		cb.check_microphone_permissions = [] {};
		cb.make_video_source = [] { return make_uwp_audio_source(); };
		cb.enable_gamemode = [](bool) {};
		cb.get_database_config = [](const std::string&) { return std::string{}; };
	}
}

bool rpcs3_embedded_is_input_allowed() { return s_initialized && !s_faulted && !s_closing; }

void embedded_console_log(std::string_view text)
{
	if (auto host = runtime()) host->log(2, 0, "stderr", std::string(text));
}

// Replace the Qt event-aware wait used by core shutdown/restart paths. This
// runs only on the core worker; UI notifications remain dispatched by pump().
void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> operation)
{
	auto host = runtime();
	if (!host || !host->is_owner()) throw std::runtime_error("Core wait on non-control thread");
	while (!operation()) host->service_internal(static_cast<uint32_t>(std::max(1, repeat_duration_ms)));
}

static void stop_and_wait()
{
	auto host = runtime();
	Emu.Kill(false);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!Emu.IsStopped(true))
	{
		// A previous guest/renderer fault must not abort the asynchronous join.
		// Keep the fault latched, but wait for full stop before allowing unload.
		if (std::chrono::steady_clock::now() >= deadline)
		{
			s_faulted = true;
			throw std::runtime_error("Core stop did not complete; do not unload the DLL");
		}
		host->service_internal(5);
	}
}

bool rpcs3_embedded_is_control_thread()
{
	auto host = runtime();
	return host && host->is_owner();
}

void rpcs3_embedded_normalize_settings()
{
	normalize_host_settings();
}

void rpcs3_embedded_report_fault(std::string_view message)
{
	s_faulted = true;
	if (auto host = runtime()) host->error(std::string(message));
}

[[noreturn]] void report_fatal_error(std::string_view message, bool, bool)
{
	rpcs3_embedded_report_fault(message);
	throw std::runtime_error(std::string(message));
}

uint32_t rpcs3_core_api_version() { return 2; }
const char* rpcs3_core_version() try
{
	static const std::string version = rpcs3::get_version().to_string();
	return version.c_str();
}
catch (...) { return "RPCS3 (version unavailable)"; }

int32_t rpcs3_core_set_callbacks(const rpcs3_core_callbacks* callbacks) try
{
	if (!callbacks || callbacks->struct_size != sizeof(*callbacks) || callbacks->api_version != 2)
		return RPCS3_CORE_INVALID_ARGUMENT;
	std::shared_ptr<rpcs3::embedded::host_runtime> host;
	{
		std::lock_guard lock(s_api_mutex);
		if (s_closing || s_init_requested) return RPCS3_CORE_BUSY;
		if (!s_runtime) s_runtime = std::make_shared<rpcs3::embedded::host_runtime>();
		host = s_runtime;
	}
	return host->set_callbacks(*callbacks);
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_initialize(const char* state_root) try
{
	if (!state_root || !*state_root) return RPCS3_CORE_INVALID_ARGUMENT;
	auto host = runtime();
	if (!host) return RPCS3_CORE_NOT_INITIALIZED;
	std::string root(state_root);
	if (!std::filesystem::path(std::u8string(root.begin(), root.end())).is_absolute()) return RPCS3_CORE_INVALID_ARGUMENT;
	if (s_init_requested.exchange(true)) return RPCS3_CORE_ALREADY_INITIALIZED;
	const auto result = host->submit(RPCS3_CORE_COMMAND_INITIALIZE, [root = std::move(root), host]() -> int32_t
	{
		try
		{
			std::filesystem::create_directories(std::filesystem::path(std::u8string(root.begin(), root.end())));
			std::filesystem::create_directories(std::filesystem::path(std::u8string(root.begin(), root.end())) / "config");
			host->open_log(root + "/RPCS3.log");
			if (!fs::set_host_config_dir(root)) throw std::runtime_error("State root already configured");
			if (!s_listener_added) { logs::listener::add(&s_listener); s_listener_added = true; }
			logs::set_init({});
			install_callbacks();
			if (!rpcs3_embedded_install_exception_handler())
				throw std::runtime_error("Cannot install guest memory exception handler");
			Emu.SetHasGui(true);
			Emu.SetHeadless(false);
			Emu.SetSupportedRenderers({video_renderer::null});
#ifdef RPCS3_UWP_MESA
			Emu.SetSupportedRenderers({video_renderer::null, video_renderer::opengl});
#endif
			Emu.SetDefaultRenderer(video_renderer::null);
			Emu.SetUsr("00000001");
			Emu.Init();
			normalize_host_settings();
			s_initialized = true;
			refresh_config_snapshot();
			state_changed();
			return RPCS3_CORE_OK;
		}
		catch (...) { s_faulted = true; throw; }
	});
	if (result != RPCS3_CORE_OK) s_init_requested = false;
	return result;
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_boot(const char* path) try
{
	if (!path || !*path) return RPCS3_CORE_INVALID_ARGUMENT;
	return queue_command(RPCS3_CORE_COMMAND_BOOT, [path = std::string(path)]
	{
		Emu.SetForceBoot(true);
		return static_cast<int32_t>(Emu.BootGame(path));
	});
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_pump() try
{
	auto host = runtime();
	return host ? host->pump() : RPCS3_CORE_NOT_INITIALIZED;
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_pause() try
{
	return queue_command(RPCS3_CORE_COMMAND_PAUSE, []
	{ return Emu.Pause() ? RPCS3_CORE_OK : RPCS3_CORE_INVALID_ARGUMENT; });
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_resume() try
{
	return queue_command(RPCS3_CORE_COMMAND_RESUME, [] { Emu.Resume(); return RPCS3_CORE_OK; });
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_stop() try
{
	return queue_command(RPCS3_CORE_COMMAND_STOP, [] { stop_and_wait(); return RPCS3_CORE_OK; });
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_shutdown() try
{
	auto host = runtime();
	if (!host || !s_init_requested) return RPCS3_CORE_NOT_INITIALIZED;
	if (s_closing.exchange(true)) return RPCS3_CORE_BUSY;
	const auto result = host->submit(RPCS3_CORE_COMMAND_SHUTDOWN, []() -> int32_t
	{
		if (s_initialized) { stop_and_wait(); Emu.CleanUp(); }
		rpcs3::embedded::clear_storage_mounts();
		d3d12::presentation().detach();
		if (!rpcs3_embedded_remove_exception_handler())
		{
			s_faulted = true;
			throw std::runtime_error("Cannot detach guest memory exception handler; do not unload the DLL");
		}
		s_initialized = false;
		state_changed();
		return s_faulted ? RPCS3_CORE_FAULTED : RPCS3_CORE_OK;
	});
	if (result != RPCS3_CORE_OK) s_closing = false;
	return result;
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_mount_storage(uint32_t kind, void* abi, const char* name, uint32_t writable,
	char* root, uint32_t capacity, uint32_t* required) try
{
	auto host = runtime();
	if (!host || !s_initialized) return RPCS3_CORE_NOT_INITIALIZED;
	if (s_faulted) return RPCS3_CORE_FAULTED;
	if (s_closing || s_state != 0 || !host->idle()) return RPCS3_CORE_BUSY;
	return rpcs3::embedded::mount_storage(kind, abi, name, writable, root, capacity, required);
}
catch (const std::exception& ex)
{
	if (auto host = runtime()) host->error(ex.what());
	return RPCS3_CORE_INTERNAL_ERROR;
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_unmount_storage(const char* name) try
{
	auto host = runtime();
	if (!host || !s_initialized) return RPCS3_CORE_NOT_INITIALIZED;
	if (s_faulted) return RPCS3_CORE_FAULTED;
	if (s_closing || s_state != 0 || !host->idle()) return RPCS3_CORE_BUSY;
	return rpcs3::embedded::unmount_storage(name);
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_release() try
{
	std::shared_ptr<rpcs3::embedded::host_runtime> host;
	{
		std::lock_guard lock(s_api_mutex);
		if (!s_runtime) return RPCS3_CORE_NOT_INITIALIZED;
		if (!s_closing || s_initialized || !s_runtime->idle() || s_faulted)
			return s_faulted ? RPCS3_CORE_FAULTED : RPCS3_CORE_BUSY;
		if (!s_runtime->prepare_release()) return RPCS3_CORE_BUSY;
		host = std::move(s_runtime);
	}
	// Final shutdown is terminal; global RPCS3 state is not reinitializable.
	host.reset();
	return RPCS3_CORE_OK;
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_eject_disc() try
{
	return queue_command(RPCS3_CORE_COMMAND_EJECT_DISC, [] { Emu.EjectDisc(); return RPCS3_CORE_OK; });
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_insert_disc(const char* path) try
{
	if (!path || !*path) return RPCS3_CORE_INVALID_ARGUMENT;
	return queue_command(RPCS3_CORE_COMMAND_INSERT_DISC, [path = std::string(path)]
	{ return static_cast<int32_t>(Emu.InsertDisc(path)); });
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_save_settings() try
{
	return queue_command(RPCS3_CORE_COMMAND_SAVE_SETTINGS, []
	{ return save_current_config() ? RPCS3_CORE_OK : RPCS3_CORE_IO_ERROR; });
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_install_package(const char* path) try
{
	if (!path || !*path) return RPCS3_CORE_INVALID_ARGUMENT;
	if (!Emu.IsStopped()) return RPCS3_CORE_BUSY;
	return queue_command(RPCS3_CORE_COMMAND_INSTALL_PACKAGE, [path = std::string(path)]
	{
		return rpcs3::utils::install_pkg(path, false) ?
			RPCS3_CORE_OK : RPCS3_CORE_INTERNAL_ERROR;
	});
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_install_firmware(const char* path) try
{
	if (!path || !*path) return RPCS3_CORE_INVALID_ARGUMENT;
	if (!Emu.IsStopped()) return RPCS3_CORE_BUSY;
	return queue_command(RPCS3_CORE_COMMAND_INSTALL_FIRMWARE, [path = std::string(path)]
	{
		return rpcs3::utils::install_firmware(path) ?
			RPCS3_CORE_OK : RPCS3_CORE_INTERNAL_ERROR;
	});
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

static int32_t copy_text(const std::string& text, char* output, uint32_t capacity, uint32_t* required)
{
	if (!required || (!output && capacity)) return RPCS3_CORE_INVALID_ARGUMENT;
	*required = static_cast<uint32_t>(text.size() + 1);
	if (capacity < *required) return RPCS3_CORE_BUFFER_TOO_SMALL;
	std::memcpy(output, text.c_str(), *required);
	return RPCS3_CORE_OK;
}

int32_t rpcs3_core_last_error(char* output, uint32_t capacity, uint32_t* required) try
{
	auto host = runtime();
	return host ? copy_text(host->last_error(), output, capacity, required) : RPCS3_CORE_NOT_INITIALIZED;
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_log_path(char* output, uint32_t capacity, uint32_t* required) try
{
	auto host = runtime();
	return host ? copy_text(host->log_path(), output, capacity, required) : RPCS3_CORE_NOT_INITIALIZED;
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

uint32_t rpcs3_core_state() { return s_state.load(); }

int32_t rpcs3_core_enumerate_config(rpcs3_core_config_callback callback, void* user) try
{
	if (!callback) return RPCS3_CORE_INVALID_ARGUMENT;
	if (!s_initialized) return RPCS3_CORE_NOT_INITIALIZED;
	std::lock_guard lock(s_config_mutex);
	for (const auto& source : s_config_snapshot)
	{
		const rpcs3_core_config_entry entry{sizeof(entry), source.type, source.flags,
			source.path.c_str(), source.name.c_str(), source.value.c_str(),
			source.default_value.c_str(), source.enum_values.c_str(), source.group.c_str(),
			source.minimum_value.c_str(), source.maximum_value.c_str(), source.restriction.c_str()};
		callback(user, &entry);
	}
	return RPCS3_CORE_OK;
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_set_config(const char* path_utf8, const char* value_utf8) try
{
	if (!path_utf8 || !*path_utf8 || !value_utf8) return RPCS3_CORE_INVALID_ARGUMENT;
	const std::string path(path_utf8);
	std::string value(value_utf8);
#ifdef RPCS3_UWP_MESA
	if (path == "Video/Renderer")
	{
		if (value == "Direct3D 12") value = "Null";
		else if (value == "OpenGL (Mesa Gallium D3D12)") value = "OpenGL";
		else return RPCS3_CORE_UNSUPPORTED_RENDERER;
	}
#endif
	if (is_uwp_locked_setting(path)) return RPCS3_CORE_UNSUPPORTED_RENDERER;
	return queue_command(RPCS3_CORE_COMMAND_SET_CONFIG, [path, value]
	{
		cfg::_base* item = find_config(path);
		if (!item || item->get_type() == cfg::type::node) return RPCS3_CORE_INVALID_ARGUMENT;
		if (!Emu.IsStopped() && !item->get_is_dynamic()) return RPCS3_CORE_BUSY;
		const auto type = item->get_type();
		const auto previous = item->to_yaml();
		const bool structured = type == cfg::type::set || type == cfg::type::map ||
			type == cfg::type::node_map || type == cfg::type::log || type == cfg::type::device;
		if (!(structured ? item->from_yaml(value, !Emu.IsStopped()) :
			item->from_string(value, !Emu.IsStopped()))) return RPCS3_CORE_INVALID_ARGUMENT;
		if (!save_current_config())
		{
			(void)item->from_yaml(previous);
			return RPCS3_CORE_IO_ERROR;
		}
		refresh_config_snapshot();
		return RPCS3_CORE_OK;
	});
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_reset_config(const char* prefix_utf8) try
{
	if (!prefix_utf8) return RPCS3_CORE_INVALID_ARGUMENT;
	const std::string prefix(prefix_utf8);
	return queue_command(RPCS3_CORE_COMMAND_RESET_CONFIG, [prefix]
	{
		cfg::_base* item = prefix.empty() ? static_cast<cfg::_base*>(&g_cfg) : find_config(prefix);
		if (!item || is_uwp_locked_setting(prefix)) return RPCS3_CORE_INVALID_ARGUMENT;
		// A node reset can contain non-dynamic descendants; only reset a coherent
		// configuration subtree while the guest is stopped.
		if (!Emu.IsStopped()) return RPCS3_CORE_BUSY;
		if (prefix == "Mounts" || prefix == "IPC") return RPCS3_CORE_INVALID_ARGUMENT;
		const auto previous = item->to_yaml();
		item->from_default();
		normalize_host_settings();
		if (!save_current_config())
		{
			(void)item->from_yaml(previous);
			return RPCS3_CORE_IO_ERROR;
		}
		refresh_config_snapshot();
		return RPCS3_CORE_OK;
	});
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_attach_d3d12(void* device, void* queue) try
{
	if (!device || !queue) return RPCS3_CORE_INVALID_ARGUMENT;
	auto host = runtime();
	if (!host || !s_initialized) return RPCS3_CORE_NOT_INITIALIZED;
	if (s_faulted) return RPCS3_CORE_FAULTED;
	if (s_closing || s_state != 0 || !host->idle()) return RPCS3_CORE_BUSY;
#ifdef RPCS3_UWP_D3D12
	d3d12::presentation().attach(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue));
	return RPCS3_CORE_OK;
#else
	return RPCS3_CORE_UNSUPPORTED_RENDERER;
#endif
}
catch (const std::exception& error)
{
	if (auto host = runtime()) host->error(error.what());
	return RPCS3_CORE_INVALID_ARGUMENT;
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_acquire_d3d12_frame(void** resource, uint64_t* serial) try
{
	if (!resource || !serial) return RPCS3_CORE_INVALID_ARGUMENT;
	*resource = nullptr;
	*serial = 0;
	if (!s_initialized) return RPCS3_CORE_NOT_INITIALIZED;
	if (s_faulted) return RPCS3_CORE_FAULTED;
	auto frame = d3d12::presentation().acquire(*serial);
	if (!frame) return RPCS3_CORE_BUSY;
	*resource = frame.Detach();
	return RPCS3_CORE_OK;
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }

int32_t rpcs3_core_log_stats(uint64_t* callback_records, uint64_t* file_records) try
{
	if (!callback_records || !file_records) return RPCS3_CORE_INVALID_ARGUMENT;
	auto host = runtime();
	if (!host) return RPCS3_CORE_NOT_INITIALIZED;
	*callback_records = host->dropped_callbacks();
	*file_records = host->dropped_file();
	return RPCS3_CORE_OK;
}
catch (...) { return RPCS3_CORE_INTERNAL_ERROR; }
