#include "MinimalHost.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <Windows.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <utility>
#ifdef RPCS3_HOST_WITH_CORE
#include "core_api.h"
#endif

namespace UwpImGuiFrontend::Sample
{
// https://screenscraper.fr/systemeinfos.php?plateforme=59
constexpr int kScreenScraperPs3SystemId = 59;
MinimalHost::MinimalHost()
{
	// Capture brokered package paths on the XAML UI apartment. The rendering
	// loop runs on a ThreadPool worker and must not repeatedly activate these
	// apartment-affine WinRT statics while processing core callbacks.
	m_resourceRoot = std::filesystem::path(
		winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()) /
		"resources";
	m_stateRoot = std::filesystem::path(
		winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str());
	m_uiDispatcher = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Dispatcher();
	std::ifstream settings(StateRoot() / "interface-settings.txt");
	std::string theme;
	bool hints = true, animations = true;
	if (settings >> std::quoted(theme) >> hints >> animations)
	{
		const auto& themes = ShellThemes();
		for (std::size_t index = 0; index < themes.size(); ++index)
			if (theme == themes[index].name) m_interfaceSettings.themeIndex = static_cast<int>(index);
		m_interfaceSettings.showActionHints = hints;
		m_interfaceSettings.animateLaunches = animations;
	}
	m_inputMappingHost.SetDsuServerEnumerator([this] {
		return m_companionModel.servers;
	});
	m_companionModel.streamingAvailable = false;
	const MappingSaveResult profiles = m_inputMappingHost.SetProfileStorePath(
		StateRoot() / "controller-profiles.json");
	if (!profiles.succeeded)
		m_notifications.Push("Controller profiles: " + profiles.error);
}

MinimalHost::~MinimalHost()
{
	SavePlayHistory();
#ifdef RPCS3_HOST_WITH_CORE
	*m_alive = false;
#endif
	if (m_contextArtwork)
		ReleaseTexture(m_contextArtwork);
	if (m_picker)
		m_picker->CancelCallbacks();
}

void MinimalHost::PersistInterfaceSettings()
{
	std::ofstream output(StateRoot() / "interface-settings.txt", std::ios::trunc);
	const auto& theme = ShellThemes()[std::clamp(m_interfaceSettings.themeIndex, 0,
		static_cast<int>(ShellThemes().size()) - 1)];
	if (!(output << std::quoted(theme.name) << '\n' << m_interfaceSettings.showActionHints << '\n'
		<< m_interfaceSettings.animateLaunches << '\n') || !output.flush())
		m_notifications.Push("Unable to save interface settings");
}

bool MinimalHost::DirectionRepeat::Update(bool held, float deltaSeconds) noexcept
{
	if (!held)
	{
		Reset();
		return false;
	}
	if (!wasHeld)
	{
		wasHeld = true;
		return true;
	}
	elapsed += std::clamp(deltaSeconds, 0.0f, 0.1f);
	if (elapsed < nextRepeat)
		return false;
	elapsed = 0.0f;
	nextRepeat = 0.08f;
	return true;
}

void MinimalHost::DirectionRepeat::Reset() noexcept
{
	wasHeld = false;
	elapsed = 0.0f;
	nextRepeat = 0.30f;
}

std::vector<LibraryItem> MinimalHost::SnapshotCatalogue()
{
	return m_catalogue;
}

RunningContent MinimalHost::GetRunningContent() const
{
	return m_runningContent;
}

DsuEndpoint MinimalHost::GetDsuEndpoint() const
{
	return m_endpoint;
}

bool MinimalHost::StartStream(const StreamRequest&)
{
	m_streamFailure = "The sample host has no encoder adapter";
	return false;
}

void MinimalHost::StopStream(bool)
{
}

std::optional<ActiveStream> MinimalHost::GetActiveStream() const
{
	return std::nullopt;
}

std::optional<std::string> MinimalHost::TakeStreamFailure()
{
	return std::exchange(m_streamFailure, std::nullopt);
}

void MinimalHost::Log(LogLevel, std::string_view)
{
}

#ifdef RPCS3_HOST_WITH_CORE
void MinimalHost::PickGameFolder()
{
	if (!m_coreReady || m_libraryLoading || m_pendingCoreLaunch || rpcs3_core_state() != 0)
	{
		m_notifications.Push("Stop RPCS3 and wait for initialization before selecting a library");
		return;
	}
	if (!m_picker) m_picker = CreatePickerService(m_uiDispatcher);
	PickerRequest request;
	request.futureAccessToken = L"rpcs3-game-library";
	(void)m_picker->PickFolder(std::move(request), {
		.completed = [this](std::unique_ptr<PickedItem> item) {
			if (item) LoadGameFolder(std::move(item->futureAccessToken));
		},
		.failed = [this](std::string_view error) { m_notifications.Push(std::string(error)); },
	});
}

void MinimalHost::PickPackage()
{
	if (!m_coreReady || rpcs3_core_state() != 0 || !m_packageMount.empty())
	{
		m_notifications.Push("Stop RPCS3 and wait for the current installation");
		return;
	}
	if (!m_picker) m_picker = CreatePickerService(m_uiDispatcher);
	PickerRequest request;
	request.fileTypes = { L".pkg", L".PKG" };
	request.futureAccessToken = L"rpcs3-package-install";
	(void)m_picker->PickFile(std::move(request), {
		.completed = [this](std::unique_ptr<PickedItem> item) {
			if (item) InstallPickedPackage(std::move(item->futureAccessToken));
		},
		.failed = [this](std::string_view error) { m_notifications.Push(std::string(error)); },
	});
}

winrt::fire_and_forget MinimalHost::InstallPickedPackage(std::wstring token)
{
	using namespace winrt::Windows::Storage;
	try
	{
		const auto file = co_await AccessCache::StorageApplicationPermissions::FutureAccessList().GetFileAsync(token);
		std::array<char, 1024> root{};
		std::uint32_t required = 0;
		const int mountResult = rpcs3_core_mount_storage(RPCS3_CORE_STORAGE_FILE,
			winrt::get_abi(file), "package-install", 0, root.data(),
			static_cast<std::uint32_t>(root.size()), &required);
		if (mountResult != RPCS3_CORE_OK)
		{
			m_notifications.Push("Unable to broker the selected package (" +
				std::to_string(mountResult) + ")");
			co_return;
		}
		m_packageMount = "package-install";
		const std::string packagePath = std::string(root.data()) + "/content";
		const int installResult = rpcs3_core_install_package(packagePath.c_str());
		if (installResult != RPCS3_CORE_OK)
		{
			(void)rpcs3_core_unmount_storage(m_packageMount.c_str());
			m_packageMount.clear();
			m_notifications.Push("Package installation was rejected (" +
				std::to_string(installResult) + ")");
			co_return;
		}
		m_notifications.Push("Installing " + winrt::to_string(file.Name()) + "...");
	}
	catch (const winrt::hresult_error& error)
	{
		m_notifications.Push("Package picker failed: " + winrt::to_string(error.message()));
	}
}

void MinimalHost::PickFirmware()
{
	if (!m_coreReady || rpcs3_core_state() != 0 || !m_firmwareMount.empty())
	{
		m_notifications.Push("Stop RPCS3 and wait for the current installation");
		return;
	}
	if (!m_picker) m_picker = CreatePickerService(m_uiDispatcher);
	PickerRequest request;
	request.fileTypes = { L".pup", L".PUP" };
	request.futureAccessToken = L"rpcs3-firmware-install";
	(void)m_picker->PickFile(std::move(request), {
		.completed = [this](std::unique_ptr<PickedItem> item) {
			if (item) InstallPickedFirmware(std::move(item->futureAccessToken));
		},
		.failed = [this](std::string_view error) { m_notifications.Push(std::string(error)); },
	});
}

winrt::fire_and_forget MinimalHost::InstallPickedFirmware(std::wstring token)
{
	using namespace winrt::Windows::Storage;
	try
	{
		const auto file = co_await AccessCache::StorageApplicationPermissions::FutureAccessList().GetFileAsync(token);
		std::array<char, 1024> root{};
		std::uint32_t required = 0;
		const int mountResult = rpcs3_core_mount_storage(RPCS3_CORE_STORAGE_FILE,
			winrt::get_abi(file), "firmware-install", 0, root.data(),
			static_cast<std::uint32_t>(root.size()), &required);
		if (mountResult != RPCS3_CORE_OK)
		{
			m_notifications.Push("Unable to broker the firmware image (" +
				std::to_string(mountResult) + ")");
			co_return;
		}
		m_firmwareMount = "firmware-install";
		const std::string path = std::string(root.data()) + "/content";
		const int result = rpcs3_core_install_firmware(path.c_str());
		if (result != RPCS3_CORE_OK)
		{
			(void)rpcs3_core_unmount_storage(m_firmwareMount.c_str());
			m_firmwareMount.clear();
			m_notifications.Push("Firmware installation was rejected (" +
				std::to_string(result) + ")");
			co_return;
		}
		m_notifications.Push("Installing " + winrt::to_string(file.Name()) + "...");
	}
	catch (const winrt::hresult_error& error)
	{
		m_notifications.Push("Firmware picker failed: " + winrt::to_string(error.message()));
	}
}

void MinimalHost::OpenStateFolder(std::wstring_view relativePath)
{
	try
	{
		const std::filesystem::path path = StateRoot() / "rpcs3" / std::filesystem::path(relativePath);
		std::filesystem::create_directories(path);
		auto operation = winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(
			winrt::hstring(path.c_str()));
		operation.Completed([](auto const& completed, auto status) {
			if (status == winrt::Windows::Foundation::AsyncStatus::Completed)
				(void)winrt::Windows::System::Launcher::LaunchFolderAsync(completed.GetResults());
		});
	}
	catch (const winrt::hresult_error& error)
	{
		m_notifications.Push("Unable to open data folder: " + winrt::to_string(error.message()));
	}
}

winrt::fire_and_forget MinimalHost::LoadGameFolder(std::wstring token)
{
	using namespace winrt::Windows::Storage;
	if (!m_coreReady || m_libraryLoading || m_pendingCoreLaunch || rpcs3_core_state() != 0) {
		m_notifications.Push("Library update requires an initialized, stopped RPCS3 core"); co_return;
	}
	const auto alive = m_alive;
	const auto dispatcher = m_uiDispatcher;
	m_libraryLoading = true;
	StorageFolder folder{nullptr};
	std::vector<LibraryItem> catalogue;
	std::string error;
	try
	{
		co_await winrt::resume_background();
		const auto access = AccessCache::StorageApplicationPermissions::FutureAccessList();
		if (access.ContainsItem(token))
		{
			folder = co_await access.GetFolderAsync(token);
			// The mounted core scanner is authoritative. A separate EBOOT walk
			// duplicated brokered I/O and produced a catalogue discarded below.
		}
	}
	catch (const winrt::hresult_error& ex) {
		char code[24]; std::snprintf(code, sizeof(code), " (0x%08X)", static_cast<unsigned>(ex.code().value));
		error = winrt::to_string(ex.message()) + code;
	}
	catch (const std::exception& ex) { error = ex.what(); }
	if (!*alive) co_return;
	try { co_await winrt::resume_foreground(dispatcher); }
	catch (...) { co_return; } // Closing CoreWindow: never terminate from fire_and_forget.
	if (!*alive) co_return;
	m_libraryLoading = false;
	if (!error.empty()) { m_notifications.Push("Library: " + error); co_return; }
	char root[256]{}; std::uint32_t required = 0;
	const auto mount = folder ? "games-" + std::to_string(++m_mountGeneration) : std::string{};
	const auto result = folder ? rpcs3_core_mount_storage(RPCS3_CORE_STORAGE_FOLDER, winrt::get_abi(folder),
		mount.c_str(), 0, root, sizeof(root), &required) : RPCS3_CORE_OK;
	if (result != RPCS3_CORE_OK) {
		m_notifications.Push("Library mount rejected: " + std::to_string(result)); co_return;
	}
	const std::string mountRoot(root);
	m_libraryLoading = true;
	const auto cache = StateRoot() / "game-icons";
	try
	{
		co_await winrt::resume_background();
		catalogue.clear();
		struct ScanContext { std::vector<LibraryItem>* items; std::filesystem::path cache; } context{&catalogue, cache};
		std::filesystem::create_directories(cache);
		const auto scanResult = rpcs3_core_enumerate_games(mountRoot.c_str(),
			[](void* opaque, const rpcs3_core_game_info* info) {
				auto& context = *static_cast<ScanContext*>(opaque);
				LibraryItem entry;
				entry.id = 14695981039346656037ull;
				for (const unsigned char c : std::string(info->serial) + info->game_dir) { entry.id ^= c; entry.id *= 1099511628211ull; }
				entry.name = info->name; entry.serial = info->serial;
				entry.appVersion = info->app_version; entry.revision = info->revision;
				entry.category = info->category; entry.firmware = info->firmware;
				entry.attributes = info->attributes; entry.bootable = info->bootable;
				entry.parentalLevel = info->parental_level; entry.resolutions = info->resolutions;
				entry.soundFormats = info->sound_formats; entry.isIso = info->is_iso != 0;
				entry.customIcon = info->custom_icon != 0; entry.gameDirectory = info->game_dir;
				entry.sizeOnDisk = info->size_on_disk; entry.customConfig = info->custom_config != 0;
				entry.customPadConfig = info->custom_pad_config != 0;
				entry.iconPath = info->icon_path; entry.moviePath = info->movie_path; entry.audioPath = info->audio_path;
				entry.platform = "PS3"; entry.format = entry.isIso ? "ISO" : "Folder";
				const std::string path(info->path);
				entry.launchPath = std::filesystem::path(std::u8string(path.begin(), path.end()));
				if (info->icon_size)
				{
					const auto icon = context.cache / (std::to_string(entry.id) + ".png");
					std::ofstream output(icon, std::ios::binary | std::ios::trunc);
					output.write(static_cast<const char*>(info->icon_data), info->icon_size);
					if (output) entry.media.push_back({MediaKind::Cover2D, icon});
				}
				context.items->push_back(std::move(entry));
			}, &context);
		if (scanResult != RPCS3_CORE_OK) error = "Metadata scan failed: " + std::to_string(scanResult);
		for (auto& game : catalogue) {
			std::ifstream history(cache / (std::to_string(game.id) + ".history"));
			history >> game.playTimeSeconds;
			history.ignore(); std::getline(history, game.lastPlayed);
		}
		std::ifstream database(cache.parent_path() / "rpcs3/GuiConfigs/compat_database.dat", std::ios::binary);
		if (database) try {
			const std::string bytes((std::istreambuf_iterator<char>(database)), {});
			winrt::Windows::Data::Json::JsonObject document;
			if (winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(bytes), document) &&
				document.GetNamedNumber(L"return_code", -255) >= 0 && document.HasKey(L"results")) {
				const auto results = document.GetNamedObject(L"results");
				for (auto& game : catalogue) if (results.HasKey(winrt::to_hstring(game.serial))) {
					const auto record = results.GetNamedObject(winrt::to_hstring(game.serial));
					game.compatibility = winrt::to_string(record.GetNamedString(L"status", L"NoResult"));
					game.compatibilityDate = winrt::to_string(record.GetNamedString(L"date", L""));
					game.latestVersion = winrt::to_string(record.GetNamedString(L"update", L""));
				}
			}
		}
		catch (const winrt::hresult_error&) { /* Optional invalid database must not discard the library. */ }
	}
	catch (const std::exception& ex) { error = ex.what(); }
	catch (...) { error = "Metadata scan failed"; }
	try { co_await winrt::resume_foreground(dispatcher); } catch (...) { co_return; }
	if (!*alive) co_return;
	m_libraryLoading = false;
	if (!error.empty()) {
		if (!mount.empty()) (void)rpcs3_core_unmount_storage(mount.c_str());
		m_notifications.Push(error); co_return;
	}
	if (!m_gameMount.empty()) (void)rpcs3_core_unmount_storage(m_gameMount.c_str());
	m_gameMount = mount;
	if (folder) m_gamesPath = std::filesystem::path(folder.Name().c_str()); // Display only, never native IO.
	SetCatalogue(std::move(catalogue));
	m_notifications.Push("Brokered library mounted: " + std::to_string(m_catalogue.size()) + " games");
}

void MinimalHost::OnCoreEvent(std::uint32_t type, std::uint32_t command,
	std::int32_t result, std::string_view message)
{
	if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && command == RPCS3_CORE_COMMAND_INITIALIZE) {
		m_coreReady = result == RPCS3_CORE_OK;
		if (m_coreReady) {
			RefreshCoreSettings();
			LoadGameFolder(L"rpcs3-game-library");
		}
	}
	if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && result == RPCS3_CORE_OK &&
		(command == RPCS3_CORE_COMMAND_SET_CONFIG ||
		 command == RPCS3_CORE_COMMAND_RESET_CONFIG))
		RefreshCoreSettings();
	if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && command == RPCS3_CORE_COMMAND_SAVE_SETTINGS && result == RPCS3_CORE_OK)
		m_notifications.Push("RPCS3 settings saved");
	if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && result == RPCS3_CORE_OK &&
		(command == RPCS3_CORE_COMMAND_BOOT || command == RPCS3_CORE_COMMAND_STOP ||
		 command == RPCS3_CORE_COMMAND_INSTALL_FIRMWARE)) RefreshCoreSettings();
	if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && command == RPCS3_CORE_COMMAND_STOP && result == RPCS3_CORE_OK)
		SavePlayHistory();
	if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && command == RPCS3_CORE_COMMAND_STOP && result == RPCS3_CORE_OK)
		m_runningContent = {};
	if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && command == RPCS3_CORE_COMMAND_SHUTDOWN)
		m_coreReady = false;
	if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE &&
		command == RPCS3_CORE_COMMAND_INSTALL_PACKAGE)
	{
		if (!m_packageMount.empty())
		{
			(void)rpcs3_core_unmount_storage(m_packageMount.c_str());
			m_packageMount.clear();
		}
		m_notifications.Push(result == RPCS3_CORE_OK ?
			"Package installed successfully" : "Package installation failed");
	}
	if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE &&
		command == RPCS3_CORE_COMMAND_INSTALL_FIRMWARE)
	{
		if (!m_firmwareMount.empty())
		{
			(void)rpcs3_core_unmount_storage(m_firmwareMount.c_str());
			m_firmwareMount.clear();
		}
		m_notifications.Push(result == RPCS3_CORE_OK ?
			"Firmware installed successfully" : "Firmware installation failed");
	}
	if (type == RPCS3_CORE_EVENT_ERROR)
		m_notifications.Push("RPCS3: " + std::string(message));
	if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && result != RPCS3_CORE_OK)
	{
		m_notifications.Push("RPCS3 command " + std::to_string(command) +
			" failed: " + std::to_string(result));
	}
	if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && command == RPCS3_CORE_COMMAND_BOOT)
	{
		if (result == RPCS3_CORE_OK && m_pendingCoreLaunch)
		{
			m_runningContent = *m_pendingCoreLaunch;
			for (auto& game : m_catalogue) if (game.id == m_runningContent.id) {
				m_playingSerial = game.serial; m_gameStarted = std::chrono::steady_clock::now();
				const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
				std::tm time{}; localtime_s(&time, &now);
				char date[32]; std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &time);
				game.lastPlayed = date;
			}
		}
		m_pendingCoreLaunch.reset();
	}
}

void MinimalHost::SavePlayHistory()
{
	if (m_playingSerial.empty()) return;
	for (auto& game : m_catalogue) if (game.serial == m_playingSerial) {
		game.playTimeSeconds += std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::steady_clock::now() - m_gameStarted).count();
		std::ofstream history(StateRoot() / "game-icons" / (std::to_string(game.id) + ".history"));
		history << game.playTimeSeconds << '\n' << game.lastPlayed << '\n';
	}
	m_playingSerial.clear();
	if (m_frontend) m_frontend->RefreshCatalogue();
}

void MinimalHost::RefreshCoreSettings()
{
	std::vector<CoreSetting> settings;
	const auto collect = [](void* user, const rpcs3_core_config_entry* entry)
	{
		if (!entry || entry->struct_size < sizeof(rpcs3_core_config_entry))
			return;
		auto& output = *static_cast<std::vector<CoreSetting>*>(user);
		CoreSetting setting;
		setting.type = entry->type;
		setting.flags = entry->flags;
		setting.path = entry->path ? entry->path : "";
		setting.name = entry->name ? entry->name : setting.path;
		setting.value = entry->value ? entry->value : "";
		setting.defaultValue = entry->default_value ? entry->default_value : "";
		setting.group = entry->group ? entry->group : "";
		setting.minimumValue = entry->minimum_value ? entry->minimum_value : "";
		setting.maximumValue = entry->maximum_value ? entry->maximum_value : "";
		setting.restriction = entry->restriction ? entry->restriction : "";
		std::string choices = entry->enum_values ? entry->enum_values : "";
		for (std::size_t offset = 0; offset <= choices.size();)
		{
			const std::size_t end = choices.find('\x1f', offset);
			setting.choices.push_back(choices.substr(offset,
				end == std::string::npos ? std::string::npos : end - offset));
			if (end == std::string::npos) break;
			offset = end + 1;
		}
		if (setting.choices.size() == 1 && setting.choices.front().empty())
			setting.choices.clear();
		output.push_back(std::move(setting));
	};
	if (rpcs3_core_enumerate_config(collect, &settings) != RPCS3_CORE_OK)
		return;
	m_coreSettings = std::move(settings);
	m_coreSettingEditors.clear();
}

void MinimalHost::SetCoreSetting(const CoreSetting& setting, std::string value)
{
	const auto result = rpcs3_core_set_config(setting.path.c_str(), value.c_str());
	if (result != RPCS3_CORE_OK)
	{
		m_notifications.Push("Setting rejected: " + setting.name + " (" +
			std::to_string(result) + ")");
		return;
	}
	// Keep the displayed snapshot in sync without replacing the vector while
	// DrawCoreSettingsPage is iterating over it. Otherwise each frame restores
	// the old slider/checkbox value despite a successful core update.
	for (auto& current : m_coreSettings)
	{
		if (current.path == setting.path)
		{
			current.value = value;
			break;
		}
	}
}

void MinimalHost::DrawCoreSettingsPage(std::string_view group)
{
	SectionTitle(std::string(group).c_str());
	ImGui::TextDisabled("RPCS3 core configuration (Qt-independent)");
	ImGui::Separator();
	ImGui::InputTextWithHint("##settings-filter", "Search settings...",
		m_coreSettingsFilter.data(), m_coreSettingsFilter.size());
	const bool stopped = rpcs3_core_state() == 0;
	for (const CoreSetting& setting : m_coreSettings)
	{
		if (setting.group != group)
			continue;
		const std::string filter(m_coreSettingsFilter.data());
		if (!filter.empty() && setting.path.find(filter) == std::string::npos)
			continue;
		const bool readOnly = (setting.flags & RPCS3_CORE_CONFIG_READ_ONLY) != 0;
		const bool dynamic = (setting.flags & RPCS3_CORE_CONFIG_DYNAMIC) != 0;
		const bool disabled = readOnly || (!stopped && !dynamic);
		ImGui::PushID(setting.path.c_str());
		const std::string label = setting.path.starts_with(std::string(group) + "/") ?
			setting.path.substr(group.size() + 1) : setting.name;
		if (disabled) ImGui::BeginDisabled();
		if (setting.type == RPCS3_CORE_CONFIG_BOOL)
		{
			bool value = setting.value == "true";
			if (ShellCheckbox(label.c_str(), &value))
				SetCoreSetting(setting, value ? "true" : "false");
		}
		else if (setting.path == "Audio/Desired Audio Buffer Duration")
		{
			int value = std::stoi(setting.value);
			const int minimum = std::stoi(setting.minimumValue);
			const int maximum = std::stoi(setting.maximumValue);
			if (ShellSliderInt(label.c_str(), &value, minimum, maximum, "%d ms"))
				SetCoreSetting(setting, std::to_string(value));
			if (ShellIntegerStepper("Fine adjustment", &value, minimum, maximum, 1, "%d ms"))
				SetCoreSetting(setting, std::to_string(value));
			ImGui::TextDisabled("Left/right adjusts by 1 ms; changes are saved immediately.");
		}
		else if (setting.type == RPCS3_CORE_CONFIG_ENUM && !setting.choices.empty())
		{
			std::vector<ChoiceItem> choices;
			choices.reserve(setting.choices.size());
			std::size_t selected = 0;
			for (std::size_t index = 0; index < setting.choices.size(); ++index)
			{
				choices.push_back({setting.choices[index], setting.choices[index], true});
				if (setting.choices[index] == setting.value) selected = index;
			}
			if (ShellCombo(label.c_str(), &selected, choices))
				SetCoreSetting(setting, setting.choices[selected]);
		}
		else
		{
			auto [editor, inserted] = m_coreSettingEditors.try_emplace(setting.path);
			if (inserted)
			{
				editor->second.resize(std::max<std::size_t>(4096, setting.value.size() + 1));
				std::copy(setting.value.begin(), setting.value.end(), editor->second.begin());
			}
			ImGui::TextUnformatted(label.c_str());
			ImGui::SetNextItemWidth(-92.0f);
			const auto resize = [](ImGuiInputTextCallbackData* data) -> int {
				if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
					auto& buffer = *static_cast<std::vector<char>*>(data->UserData);
					buffer.resize(static_cast<std::size_t>(data->BufSize));
					data->Buf = buffer.data();
				}
				return 0;
			};
			if (setting.type == RPCS3_CORE_CONFIG_COLLECTION)
				ImGui::InputTextMultiline("##value", editor->second.data(), editor->second.size(),
					{ -92.0f, 120.0f }, ImGuiInputTextFlags_CallbackResize, resize, &editor->second);
			else
				ImGui::InputText("##value", editor->second.data(), editor->second.size(),
					ImGuiInputTextFlags_CallbackResize, resize, &editor->second);
			ImGui::SameLine();
			if (ShellButton("Apply", {84.0f, 0.0f}))
				SetCoreSetting(setting, editor->second.data());
		}
		if (disabled) ImGui::EndDisabled();
		if (!setting.minimumValue.empty())
			ImGui::TextDisabled("Range: %s to %s", setting.minimumValue.c_str(), setting.maximumValue.c_str());
		if (!readOnly)
		{
			ImGui::TextDisabled("Default: %s", setting.defaultValue.c_str());
			if (stopped && ShellButton("Restore default", { 150.0f, 0.0f }))
				SetCoreSetting(setting, setting.defaultValue);
		}
		if (readOnly) ImGui::TextWrapped("%s", setting.restriction.c_str());
		else if (!stopped && !dynamic) ImGui::TextDisabled("Stop emulation to change");
		ImGui::PopID();
	}
	ImGui::Separator();
	ImGui::BeginDisabled(!stopped || group == "Mounts" || group == "IPC");
	if (ShellButton("Reset this page"))
	{
		const std::string prefix(group);
		const auto result = rpcs3_core_reset_config(prefix.c_str());
		if (result != RPCS3_CORE_OK)
			m_notifications.Push("Reset rejected: " + std::to_string(result));
	}
	ImGui::EndDisabled();
}
#endif

std::filesystem::path MinimalHost::ResourceRoot() const
{
	return m_resourceRoot;
}

std::filesystem::path MinimalHost::StateRoot() const
{
	return m_stateRoot;
}

TextureHandle MinimalHost::LoadTexture(const std::filesystem::path& path)
{
	return m_loadTexture ? m_loadTexture(path) : TextureHandle{};
}

void MinimalHost::ReleaseTexture(TextureHandle texture)
{
	if (m_releaseTexture)
		m_releaseTexture(texture);
}

bool MinimalHost::RequestLaunch(ItemId item)
{
	const auto found = std::find_if(m_catalogue.begin(), m_catalogue.end(),
		[item](const LibraryItem& entry) { return entry.id == item; });
	if (found == m_catalogue.end())
		return false;
#ifdef RPCS3_HOST_WITH_CORE
	if (!m_coreReady || m_libraryLoading || m_pendingCoreLaunch) return false;
	if (const MappingSaveResult routes = m_inputMappingHost.ActivateProfileRoutes(found->id); !routes.succeeded)
	{
		m_notifications.Push("Controller routes: " + routes.error);
		return false;
	}
	const auto path = found->launchPath.generic_u8string();
	const auto result = rpcs3_core_boot(reinterpret_cast<const char*>(path.c_str()));
	if (result != RPCS3_CORE_OK)
	{
		m_notifications.Push("RPCS3 boot request rejected: " + std::to_string(result));
		return false;
	}
	m_pendingCoreLaunch = RunningContent{ found->id, found->name, true };
#else
	if (const MappingSaveResult routes =
		m_inputMappingHost.ActivateProfileRoutes(found->id); !routes.succeeded)
	{
		m_notifications.Push("Controller routes: " + routes.error);
		return false;
	}

	m_runningContent = { found->id, found->name, true };
#endif
	return true;
}

void MinimalHost::OpenContextMenu(ItemId item)
{
	if (m_contextArtwork)
	{
		ReleaseTexture(m_contextArtwork);
		m_contextArtwork = {};
	}
	m_contextItem = item;
	m_titleOptionsState = {};
	const auto found = std::ranges::find_if(m_catalogue,
		[item](const LibraryItem& entry) { return entry.id == item; });
	if (found != m_catalogue.end())
	{
		const MediaAsset* artwork = FindMedia(*found, MediaKind::Cover2D);
		if (!artwork)
			artwork = FindMedia(*found, MediaKind::Cover3D);
		if (artwork)
			m_contextArtwork = LoadTexture(artwork->path);
	}
}

void MinimalHost::Execute(HostCommand command)
{
	switch (command)
	{
	case HostCommand::RefreshContent:
		LoadGameFolder(L"rpcs3-game-library");
		break;
	case HostCommand::AddContent:
		m_activePage = "library";
		m_libraryView.OpenAddGamesFolder();
		break;
	case HostCommand::OpenHome:
		m_activePage.clear();
		m_inputMapper.Close();
		m_mapperScope = MapperScope::None;
		CloseContextMenu();
		break;
	case HostCommand::OpenSettings:
		m_activePage = "settings";
		break;
	case HostCommand::OpenControllers:
		m_activePage = "controllers";
		OpenInputMapper(std::nullopt);
		break;
	case HostCommand::OpenGraphics:
		m_activePage = "graphics";
		break;
	case HostCommand::OpenTitleManager:
		m_activePage = "library";
		break;
	case HostCommand::OpenTools:
		m_activePage = "tools";
		break;
	case HostCommand::CycleTheme:
		m_interfaceSettings.themeIndex = (m_interfaceSettings.themeIndex + 1) %
			static_cast<int>(ShellThemes().size());
		m_notifications.Push(std::string("Theme: ") +
			ShellThemes()[m_interfaceSettings.themeIndex].name);
		PersistInterfaceSettings();
		break;
	case HostCommand::ExitApplication:
		winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
		break;
	case HostCommand::StopContent:
#ifdef RPCS3_HOST_WITH_CORE
		(void)rpcs3_core_stop();
#endif
		m_runningContent = {};
		if (const MappingSaveResult routes =
			m_inputMappingHost.ActivateProfileRoutes(std::nullopt); !routes.succeeded)
		{
			m_notifications.Push("Controller routes: " + routes.error);
		}
		break;
	case HostCommand::PauseContent:
#ifdef RPCS3_HOST_WITH_CORE
		if (rpcs3_core_pause() != RPCS3_CORE_OK)
			m_notifications.Push("Pause request rejected");
#endif
		break;
	case HostCommand::ResumeContent:
#ifdef RPCS3_HOST_WITH_CORE
		if (rpcs3_core_resume() != RPCS3_CORE_OK)
			m_notifications.Push("Resume request rejected");
#endif
		break;
	case HostCommand::EjectDisc:
#ifdef RPCS3_HOST_WITH_CORE
		if (rpcs3_core_eject_disc() != RPCS3_CORE_OK)
			m_notifications.Push("Eject request rejected");
#endif
		break;
	case HostCommand::SaveSettings:
#ifdef RPCS3_HOST_WITH_CORE
		if (rpcs3_core_save_settings() != RPCS3_CORE_OK)
			m_notifications.Push("Save settings request rejected");
#endif
		break;
	default:
		break;
	}
}

void MinimalHost::DrawSettingsPage(std::string_view pageId)
{
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	if (!viewport)
		return;
	const ShellLayoutContext context = MakeShellLayout(viewport->WorkSize,
		viewport->WorkPos);
	const float panelInset = 32.0f;
	UnitRect panel{ panelInset, panelInset,
		context.layoutUnitsW - panelInset * 2.0f,
		context.layoutUnitsH - panelInset * 2.0f };
	if (pageId == "context")
	{
		const bool metadataSearch = m_metadataEditor && m_metadataEditor->IsOpen() &&
			m_metadataEditor->View() == MetadataEditorView::MatchBrowser;
		const bool metadataOpen = m_metadataEditor && m_metadataEditor->IsOpen();
		if (metadataOpen)
		{
			const float requestedWidth = metadataSearch ?
				context.layoutUnitsW - 36.0f : 1080.0f;
			const float width = std::min(requestedWidth,
				std::max(420.0f, context.layoutUnitsW -
					(metadataSearch ? 36.0f : 80.0f)));
			const float requestedHeight = metadataSearch ?
				context.layoutUnitsH - 32.0f : 650.0f;
			const float height = std::min(requestedHeight,
				std::max(360.0f, context.layoutUnitsH -
					(metadataSearch ? 32.0f : 48.0f)));
			panel = CenteredPanel(context, width, height);
		}
	}
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBackground;
	if (!ImGui::Begin("Reusable frontend gallery", nullptr, flags))
	{
		ImGui::End();
		return;
	}
	ImDrawList* draw = ImGui::GetWindowDrawList();
	if ((pageId == "controllers" && m_mapperScope == MapperScope::Global) ||
		(pageId == "context" && m_mapperScope == MapperScope::Title))
	{
		DrawInputMapper(context, draw);
		const auto notifications = m_notifications.Snapshot();
		DrawNotificationCards(draw, context, CurrentWidgetTheme(), notifications);
		ImGui::End();
		return;
	}
	DrawDimmedBackground();
	if (pageId == "context")
	{
		if (m_metadataEditor && m_metadataEditor->IsOpen())
			DrawOverlayPanel(draw, context, panel, CurrentWidgetTheme());
		DrawContextMenu(context, draw, panel);
		const auto notifications = m_notifications.Snapshot();
		DrawNotificationCards(draw, context, CurrentWidgetTheme(), notifications);
		ImGui::End();
		return;
	}
	DrawOverlayPanel(draw, context, panel, CurrentWidgetTheme(), 0.96f);
	const UnitRect content = panel.Shrunk(30.0f);
	ImGui::SetCursorScreenPos(RectMinPx(context, content));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
	ImGui::BeginChild("sample-overlay-content", ToPx(context, content.w, content.h),
		false, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NavFlattened |
			ImGuiWindowFlags_NoBackground);
	if (pageId == "settings")
	{
		const auto storagePresets = MakeXboxStorageLocationPresets(
			L"D:\\DevelopmentFiles\\SampleFrontend", L"E:\\SampleFrontend");
		SettingsViewModel model;
		model.interfaceSettings = m_interfaceSettings;
		model.showStorage = false; // The core data root is fixed; a picker does not relocate it.
		model.screenScraper = m_screenScraperModel;
		model.companion = m_companionModel;
		StorageSettingsEntry storage;
		storage.model.id = "sample-storage";
		storage.model.label = "Application storage";
		storage.model.currentPath = m_storagePath;
		storage.model.presets = storagePresets;
		storage.model.customPicker.futureAccessToken = L"sample-storage-folder";
		storage.callbacks.selected = [this](StorageLocationSelection selection) {
			m_storagePath = std::move(selection.path);
			m_notifications.Push("Storage location updated");
		};
		storage.callbacks.failed = [this](std::string_view message) {
			m_notifications.Push(std::string(message));
		};
		model.storage.push_back(std::move(storage));
		std::vector<SettingsExtensionPage> corePages;
#ifdef RPCS3_HOST_WITH_CORE
		std::unordered_set<std::string> groups;
		for (const CoreSetting& setting : m_coreSettings)
		{
			groups.insert(setting.group);
		}
		std::vector<std::string> orderedGroups(groups.begin(), groups.end());
		std::ranges::sort(orderedGroups);
		for (const std::string& group : orderedGroups)
		{
			corePages.push_back({
				.id = "rpcs3-" + group,
				.label = group,
				.enabled = m_coreReady,
				.draw = [this, group] { DrawCoreSettingsPage(group); },
			});
		}
#endif
		if (!m_pendingSettingsPage.empty())
		{
			m_settingsView.SelectPage(m_pendingSettingsPage, model, corePages);
			m_pendingSettingsPage.clear();
		}
		m_settingsView.Draw(model, {
			.screenScraper = m_screenScraperService,
			.credentials = m_credentials.get(),
			.picker = m_picker.get(),
		}, {
			.persistInterface = [this](const InterfaceSettingsModel& value) {
				m_interfaceSettings = value;
				PersistInterfaceSettings();
				if (m_frontend)
					m_frontend->SetAnimateLaunches(value.animateLaunches);
			},
			.screenScraper = {
				.makeRequest = [this](ScreenScraperCredentials account,
					ScreenScraperOptions options) {
					ScreenScraperRequest request;
					request.account = std::move(account);
					request.options = options;
					request.platformSystemId = kScreenScraperPs3SystemId;
					request.platformMediaDirectory = StateRoot() / "media";
					request.progressFile = StateRoot() / "scrape-progress.json";
					for (const LibraryItem& item : m_catalogue)
					{
						request.targets.push_back({
							.id = item.id,
							.systemId = request.platformSystemId,
							.name = item.name,
							.region = item.region,
							.contentPath = item.launchPath,
							.mediaDirectory = request.platformMediaDirectory /
								std::to_string(item.id),
							.esDeMedia = MakeEsDeMediaLayout(
								m_screenScraperModel.mediaDirectory,
								"sample", item.launchPath),
						});
					}
					return request;
				},
				.persist = [this](const ScreenScraperPanelModel& value) {
					m_screenScraperModel = value;
				},
				.notify = [this](std::string_view message) {
					m_notifications.Push(std::string(message));
				},
				.mediaDirectorySelected = [this](StorageLocationSelection selection) {
					m_screenScraperModel.mediaDirectory = std::move(selection.path);
					m_notifications.Push("ScreenScraper media location updated");
				},
			},
				.persistCompanion = [this](const CompanionPanelModel& value) {
					m_companionModel = value;
					m_endpoint = MakeCompanionEndpoint(value);
			},
		}, corePages, ImGui::GetContentRegionAvail());
		m_interfaceSettings = model.interfaceSettings;
		m_screenScraperModel = model.screenScraper;
		m_companionModel = model.companion;
	}
	else if (pageId == "library")
	{
		const auto gameFolderPresets = MakeXboxStorageLocationPresets(
			L"D:\\DevelopmentFiles\\Games", L"E:\\Games");
		StorageLocationSelectorModel gamesFolderSelector;
		gamesFolderSelector.id = "sample-game-library";
		gamesFolderSelector.label = "Games directory";
		gamesFolderSelector.currentPath = m_gamesPath;
		gamesFolderSelector.presets = gameFolderPresets;
		gamesFolderSelector.promptLabel = "Select storage location";
		gamesFolderSelector.currentPathLabel = "Folder";
		gamesFolderSelector.customPicker.futureAccessToken =
			L"rpcs3-game-library";
		(void)m_libraryView.Draw({
			.items = m_catalogue,
			.contextRequested = std::exchange(m_libraryContextRequested, false),
			.emptyLabel = "No library items found.",
			.gamesFolderSelector = gamesFolderSelector,
			.picker = m_picker.get(),
		}, {
			.launch = [this](const LibraryItem& item) {
				(void)RequestLaunch(item.id);
			},
			.openContextMenu = [this](const LibraryItem& item) {
				OpenContextMenu(item.id);
			},
			.selectionChanged = [this](const LibraryItem& item) {
				m_notifications.Push("Selected " + item.name);
			},
			.addContent = [this] {
#ifdef RPCS3_HOST_WITH_CORE
				PickGameFolder();
#else
				m_notifications.Push("Storage location selection is unavailable");
#endif
			},
			.refresh = [this] {
#ifdef RPCS3_HOST_WITH_CORE
				LoadGameFolder(L"rpcs3-game-library");
#else
				m_notifications.Push("Library refreshed");
#endif
			},
			.addGamesFolder = {
				.selected = [this](StorageLocationSelection selection) {
#ifdef RPCS3_HOST_WITH_CORE
					if (selection.custom && !selection.futureAccessToken.empty())
						LoadGameFolder(std::move(selection.futureAccessToken));
					else PickGameFolder(); // A preset path is not an access grant.
#else
					m_gamesPath = std::move(selection.path);
					m_notifications.Push("Games directory updated");
#endif
				},
				.failed = [this](std::string_view message) {
					m_notifications.Push(std::string(message));
				},
			},
		}, ImGui::GetContentRegionAvail());
	}
	else if (pageId == "graphics")
	{
		DrawGraphicsPage();
	}
	else if (pageId == "tools")
	{
		DrawToolsPage();
	}
	else
	{
		DrawComponentGallery(context, draw);
	}
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	if (ConsumeWidgetBackPressed())
		m_activePage.clear();
	const auto notifications = m_notifications.Snapshot();
	DrawNotificationCards(draw, context, CurrentWidgetTheme(), notifications);
	ImGui::End();
}

void MinimalHost::DrawGraphicsPage()
{
#ifdef RPCS3_HOST_WITH_CORE
	if (m_coreReady) DrawCoreSettingsPage("Video");
	else ImGui::TextDisabled("Waiting for the RPCS3 core to initialize...");
#else
	const GraphicsViewModel model;
	const GraphicsViewResult result = m_graphicsView.Draw(model,
		ImGui::GetContentRegionAvail());
	if (result.closeRequested)
		m_activePage.clear();
#endif
}

void MinimalHost::DrawToolsPage()
{
	ImGui::SetScrollX(0.0f);
	SectionTitle("RPCS3 Tools");
	ImGui::TextDisabled("UWP-native administration backed by the embedded RPCS3 core");
	ImGui::Separator();

#ifdef RPCS3_HOST_WITH_CORE
	const auto coreState = rpcs3_core_state();
	ImGui::BeginDisabled(!m_coreReady || coreState != 3); // system_state::running
#endif
	const ImVec2 executionButtonSize{std::max(1.0f, (ImGui::GetContentRegionAvail().x - 4.0f * ImGui::GetStyle().ItemSpacing.x) / 5.0f), 0.0f};
	if (ShellButton("Pause", executionButtonSize)) Execute(HostCommand::PauseContent);
#ifdef RPCS3_HOST_WITH_CORE
	ImGui::EndDisabled();
#endif
	ImGui::SameLine();
#ifdef RPCS3_HOST_WITH_CORE
	ImGui::BeginDisabled(!m_coreReady || coreState != 4); // system_state::paused
#endif
	if (ShellButton("Resume", executionButtonSize)) Execute(HostCommand::ResumeContent);
#ifdef RPCS3_HOST_WITH_CORE
	ImGui::EndDisabled();
#endif
	ImGui::SameLine();
	if (ShellButton("Stop", executionButtonSize)) Execute(HostCommand::StopContent);
	ImGui::SameLine();
	if (ShellButton("Eject disc", executionButtonSize)) Execute(HostCommand::EjectDisc);
	ImGui::SameLine();
	if (ShellButton("Save settings", executionButtonSize)) Execute(HostCommand::SaveSettings);

	const float toolWidth = std::max(1.0f, (ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f);
	const auto toolButton = [toolWidth](const char* label) {
		return ShellButton(label, { toolWidth, 0.0f });
	};
	const auto nextColumn = [](int index) { if ((index % 3) != 2) ImGui::SameLine(); };
	const auto settings = [this](std::string page) {
		m_pendingSettingsPage = "rpcs3-" + std::move(page);
		m_activePage = "settings";
	};

	ImGui::Spacing();
	ImGui::TextUnformatted("Installation");
	ImGui::Separator();
	if (toolButton("Install PS3 firmware (.PUP)")) PickFirmware();
	ImGui::SameLine();
	if (toolButton("Install package or update (.PKG)")) PickPackage();

	ImGui::Spacing();
	ImGui::TextUnformatted("Data managers");
	ImGui::Separator();
	int column = 0;
	if (toolButton("Users")) OpenStateFolder(L"dev_hdd0/home"); nextColumn(column++);
	if (toolButton("Save data")) OpenStateFolder(L"dev_hdd0/home"); nextColumn(column++);
	if (toolButton("Trophies")) OpenStateFolder(L"dev_hdd0/home"); nextColumn(column++);
	if (toolButton("Savestates")) OpenStateFolder(L"savestates"); nextColumn(column++);
	if (toolButton("Screenshots")) OpenStateFolder(L"screenshots"); nextColumn(column++);
	if (toolButton("Patches")) OpenStateFolder(L"patches"); nextColumn(column++);
	if (toolButton("Cheats")) OpenStateFolder(L"cheats"); nextColumn(column++);
	if (toolButton("Virtual file system")) OpenStateFolder(L""); nextColumn(column++);
	if (toolButton("Logs and TTY")) OpenStateFolder(L""); nextColumn(column++);

	ImGui::Spacing();
	ImGui::TextUnformatted("Configuration");
	ImGui::Separator();
	column = 0;
	if (toolButton("CPU and decoders")) settings("Core"); nextColumn(column++);
	if (toolButton("Graphics and RSX")) settings("Video"); nextColumn(column++);
	if (toolButton("Audio and music")) settings("Audio"); nextColumn(column++);
	if (toolButton("Pads, mouse and camera")) settings("Input/Output"); nextColumn(column++);
	if (toolButton("RPCN and network")) settings("Net"); nextColumn(column++);
	if (toolButton("Savestate behavior")) settings("Savestate"); nextColumn(column++);
	if (toolButton("VFS mounts")) settings("Mounts"); nextColumn(column++);
	if (toolButton("IPC settings")) settings("IPC"); nextColumn(column++);
	if (toolButton("System and users")) settings("System"); nextColumn(column++);
	if (toolButton("Auto-pause and miscellaneous")) settings("Miscellaneous"); nextColumn(column++);
	if (column % 3) ImGui::NewLine();

	ImGui::Spacing();
	ImGui::TextUnformatted("Diagnostics");
	ImGui::Separator();
#ifdef RPCS3_HOST_WITH_CORE
	ImGui::Text("Core: %s", rpcs3_core_version());
	ImGui::Text("State: %u", rpcs3_core_state());
	std::uint64_t callbackDrops = 0, fileDrops = 0;
	if (rpcs3_core_log_stats(&callbackDrops, &fileDrops) == RPCS3_CORE_OK)
		ImGui::Text("Dropped log records: callback=%llu file=%llu",
			static_cast<unsigned long long>(callbackDrops),
			static_cast<unsigned long long>(fileDrops));
#endif
	ImGui::TextWrapped("Qt-only process debuggers and desktop device backends are intentionally not loaded in the AppContainer. Runtime diagnostics use the exported core state, event and log APIs instead.");
	if (toolButton("RPCS3 website and documentation")) OpenUri("https://rpcs3.net/quickstart");
}

void MinimalHost::OpenUri(std::string_view uri)
{
	if (uri.empty())
		return;
	try
	{
		(void)winrt::Windows::System::Launcher::LaunchUriAsync(
			winrt::Windows::Foundation::Uri(winrt::to_hstring(uri)));
	}
	catch (const winrt::hresult_error& error)
	{
		m_notifications.Push("Unable to open URI: " +
			winrt::to_string(error.message()));
	}
}

void MinimalHost::PersistFrontendState(std::string_view data)
{
	std::ofstream output(StateRoot() / "frontend-state.txt", std::ios::binary | std::ios::trunc);
	output.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string MinimalHost::LoadFrontendState() const
{
	std::ifstream input(StateRoot() / "frontend-state.txt", std::ios::binary);
	return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

void MinimalHost::SetCatalogue(std::vector<LibraryItem> catalogue)
{
	m_catalogue = std::move(catalogue);
	if (m_frontend) m_frontend->RefreshCatalogue();
}

void MinimalHost::SetDsuEndpoint(DsuEndpoint endpoint)
{
	m_endpoint = std::move(endpoint);
	m_companionModel.servers = {{
		.id = "default",
		.name = "DSU Server",
		.host = m_endpoint.host,
		.port = m_endpoint.port,
		.enabled = m_endpoint.enabled,
	}};
	m_companionModel.streamingEnabled = m_endpoint.streamingEnabled;
	m_companionModel.streamingUrl = m_endpoint.streamingUrl;
}

void MinimalHost::AttachScreenScraper(IScreenScraperService& service)
{
	m_screenScraperService = &service;
	m_screenScraperModel.progressFile = StateRoot() / "scrape-progress.json";
	m_screenScraperModel.mediaDirectory = L"E:\\ES-DE\\downloaded_media";
	const auto mediaPresets = MakeXboxStorageLocationPresets(
		L"D:\\DevelopmentFiles\\ES-DE\\downloaded_media",
		L"E:\\ES-DE\\downloaded_media");
	m_screenScraperModel.mediaDirectoryPresets.assign(
		mediaPresets.begin(), mediaPresets.end());
	m_screenScraperModel.mediaDirectoryPicker.futureAccessToken =
		L"sample-screenscraper-media";
	m_metadataEditor = std::make_unique<MetadataEditor>(service);
	if (!m_credentials)
		m_credentials = CreatePasswordVaultCredentialStore(
			L"UwpImGuiFrontend.Sample.ScreenScraper");
	if (!m_picker)
		m_picker = CreatePickerService(m_uiDispatcher);
}

void MinimalHost::SetTextureCallbacks(
	std::function<TextureHandle(const std::filesystem::path&)> load,
	std::function<void(TextureHandle)> release)
{
	m_inputMappingHost.SetControllerPreviewCallbacks(
		ResourceRoot() / "controller-previews", load, release);
	m_loadTexture = std::move(load);
	m_releaseTexture = std::move(release);
}

void MinimalHost::SetInputCallbacks(
	SampleInputMappingHost::DeviceEnumerator enumerate,
	SampleInputMappingHost::LiveInputReader read)
{
	m_inputMappingHost.SetInputCallbacks(std::move(enumerate), std::move(read));
}

bool MinimalHost::HandleOverlayInput(const FrameInput& input,
	const FrameInput& previousInput)
{
	if (!OwnsOverlayInput())
		return false;
	m_contextDetailScroll = m_contextItem &&
		(!m_metadataEditor || !m_metadataEditor->IsOpen()) ?
		-input.rightStickY * 520.0f * input.deltaSeconds : 0.0f;

	if (m_mapperScope != MapperScope::None)
	{
		const bool up = input.up || input.leftStickY >= 0.55f;
		const bool down = input.down || input.leftStickY <= -0.55f;
		const bool left = input.left || input.leftStickX <= -0.55f;
		const bool right = input.right || input.leftStickX >= 0.55f;
		m_mapperFrameInput = {
			.deltaSeconds = input.deltaSeconds,
			.up = m_mapperRepeat[0].Update(up, input.deltaSeconds),
			.down = m_mapperRepeat[1].Update(down, input.deltaSeconds),
			.left = m_mapperRepeat[2].Update(left, input.deltaSeconds),
			.right = m_mapperRepeat[3].Update(right, input.deltaSeconds),
			.accept = input.accept && !previousInput.accept,
			.back = input.back && !previousInput.back,
			.context = input.context && !previousInput.context,
			.alternate = input.alternate && !previousInput.alternate,
			.menu = input.menu && !previousInput.menu,
			.view = input.view && !previousInput.view,
			.leftShoulder = input.leftShoulder && !previousInput.leftShoulder,
			.rightShoulder = input.rightShoulder && !previousInput.rightShoulder,
			.controllerInputHeld = input.controllerInputHeld,
			.leftStickX = input.leftStickX,
			.leftStickY = input.leftStickY,
			.rightStickX = input.rightStickX,
			.rightStickY = input.rightStickY,
			.pointer = input.pointer,
			.pointerPressed = input.pointerPressed,
		};
		return true;
	}
	for (auto& repeat : m_mapperRepeat)
		repeat.Reset();

	if (m_contextItem && (!m_metadataEditor || !m_metadataEditor->IsOpen()) &&
		input.back && !previousInput.back)
	{
		// Consume B before it reaches shell navigation.
		(void)ConsumeWidgetBackPressed();
		CloseContextMenu();
		return true;
	}

	if (m_activePage == "library" && input.context && !previousInput.context)
	{
		m_libraryContextRequested = true;
		return true;
	}
	if (m_metadataEditor && m_metadataEditor->IsOpen())
	{
		const MetadataEditorEvent event = m_metadataEditor->HandleInput({
			.accept = input.accept && !previousInput.accept,
			.back = input.back && !previousInput.back,
			.up = input.up && !previousInput.up,
			.down = input.down && !previousInput.down,
			.left = input.left && !previousInput.left,
			.right = input.right && !previousInput.right,
			.leftShoulder = input.leftShoulder && !previousInput.leftShoulder,
			.rightShoulder = input.rightShoulder && !previousInput.rightShoulder,
			.rightStickY = input.rightStickY,
			.deltaSeconds = input.deltaSeconds,
		});
		if (event != MetadataEditorEvent::None)
		{
			// Consume the editor's B before drawing title options.
			(void)ConsumeWidgetBackPressed();
		}
	}
	return true;
}

void MinimalHost::DrawOverlayPages()
{
	if (m_activePage.empty() && !m_contextItem)
		return;
	DrawSettingsPage(m_contextItem ? "context" : m_activePage);
}

void MinimalHost::DrawComponentGallery(const ShellLayoutContext& context,
	ImDrawList* draw)
{
	const std::array tabs{
		SplitViewTab{ "controls", "Controls" },
		SplitViewTab{ "library", "Data table" },
		SplitViewTab{ "services", "Services" },
		SplitViewTab{ "dialogs", "Dialogs" },
	};
	DrawSplitView("sample-gallery", m_splitState, tabs,
		[this](std::size_t page) {
			switch (page)
			{
			case 0:
			{
				SectionTitle("Form controls");
				(void)NativeInputText("Display name", m_displayName.data(),
					m_displayName.size());
				(void)ShellCheckbox("Feature enabled", &m_featureEnabled);
				(void)ShellSliderFloat("Interface scale", &m_interfaceScale,
					0.75f, 1.5f, "%.2f");
				(void)ShellSliderInt("Cache limit", &m_cacheLimit, 64, 2048);
				(void)ShellIntegerStepper("Worker count", &m_workerCount, 1, 16);
				const std::array choices{
					ChoiceItem{ "balanced", "Balanced" },
					ChoiceItem{ "quality", "Quality" },
					ChoiceItem{ "performance", "Performance" },
				};
				(void)ShellCombo("Preset", &m_quality, choices);
				(void)ShellColorEdit("Accent color", &m_accent);
				ShellProgressBar(0.68f, "68%");
				break;
			}
			case 1:
			{
				const std::array columns{
					DataTableColumn{ "art", "", DataTableColumnKind::Artwork, 78.0f },
					DataTableColumn{ "name", "Name", DataTableColumnKind::PrimaryText,
						2.4f, true },
					DataTableColumn{ "platform", "Platform", DataTableColumnKind::Text,
						1.0f, true },
					DataTableColumn{ "action", "Action", DataTableColumnKind::Action,
						104.0f },
				};
				std::vector<DataTableRow> rows;
				rows.reserve(m_catalogue.size());
				for (const LibraryItem& item : m_catalogue)
				{
					rows.push_back({ item.id, {
						{ {}, {} },
						{ item.name },
						{ item.platform },
						{ "Open" },
					} });
				}
				if (rows.empty())
					ImGui::TextDisabled("The sample catalogue is empty.");
				else if (const auto result = DrawDataTable("sample-data-table",
					m_tableState, columns, rows); result.activatedRow)
				{
					m_notifications.Push("Selected " +
						m_catalogue[*result.activatedRow].name);
				}
				break;
			}
			case 2:
				if (m_screenScraperService && m_credentials)
				{
					m_screenScraperPanel.Draw(m_screenScraperModel,
						*m_screenScraperService, *m_credentials, {
							.makeRequest = [this](ScreenScraperCredentials account,
								ScreenScraperOptions options) {
								ScreenScraperRequest request;
								request.account = std::move(account);
								request.options = options;
								request.platformSystemId = kScreenScraperPs3SystemId;
								request.platformMediaDirectory = StateRoot() / "media";
								request.progressFile = StateRoot() / "scrape-progress.json";
								for (const LibraryItem& item : m_catalogue)
								{
									request.targets.push_back({
										.id = item.id,
										.systemId = request.platformSystemId,
										.matchedGameId = 0,
										.name = item.name,
										.region = item.region,
										.contentPath = item.launchPath,
										.mediaDirectory = request.platformMediaDirectory /
											std::to_string(item.id),
										.esDeMedia = MakeEsDeMediaLayout(
											m_screenScraperModel.mediaDirectory,
											"sample", item.launchPath),
									});
								}
								return request;
							},
							.persist = [this](const ScreenScraperPanelModel& value) {
								m_screenScraperModel = value;
							},
							.notify = [this](std::string_view message) {
								m_notifications.Push(std::string(message));
							},
							.mediaDirectorySelected = [this](StorageLocationSelection selection) {
								m_screenScraperModel.mediaDirectory = std::move(selection.path);
								m_notifications.Push("ScreenScraper media location updated");
							},
						}, m_picker.get());
				}
				SectionTitle("DSU & streaming");
				m_companionPanel.Draw(m_companionModel,
						[this](const CompanionPanelModel& model) {
							m_endpoint = MakeCompanionEndpoint(model);
					});
				break;
			case 3:
			{
				SectionTitle("Storage location");
				const std::array storagePresets{
					StorageLocationPreset{ "development", "Development drive",
						L"D:\\DevelopmentFiles\\SampleFrontend" },
					StorageLocationPreset{ "external", "E: drive",
						L"E:\\SampleFrontend" },
				};
				StorageLocationSelectorModel storageModel;
				storageModel.id = "sample-storage";
				storageModel.currentPath = m_storagePath;
				storageModel.presets = storagePresets;
				storageModel.customPicker.futureAccessToken = L"sample-storage-folder";
				if (m_picker)
				{
					(void)DrawStorageLocationSelector(storageModel, *m_picker, {
						.selected = [this](StorageLocationSelection selection) {
							m_storagePath = std::move(selection.path);
							m_notifications.Push("Storage location updated");
						},
						.failed = [this](std::string_view message) {
							m_notifications.Push(std::string(message));
						},
					});
				}
				SectionTitle("Dialogs and platform services");
				if (ShellButton("Open selection dialog"))
					m_showSelectionDialog = true;
				if (ShellButton("Open confirmation"))
					m_showConfirmation = true;
				if (ShellButton("Open text editor"))
				{
					std::strncpy(m_editorBuffer.data(), m_displayName.data(),
						m_editorBuffer.size() - 1);
					m_editorBuffer.back() = '\0';
					m_textEditorState.requestFocus = true;
					m_showTextEditor = true;
				}
				if (ShellButton("Open quick menu"))
					m_showQuickMenu = true;
				if (ShellButton("Open form editor"))
					m_showFormEditor = true;
				if (ShellButton("Open progress dialog"))
					m_showProgressDialog = true;
				if (ShellButton("Pick a folder") && m_picker && !m_picker->IsOpen())
				{
					(void)m_picker->PickFolder({}, {
						.completed = [this](std::unique_ptr<PickedItem> item) {
							m_notifications.Push(item ?
								"Selected " + item->path.string() : "Folder picker cancelled");
						},
						.failed = [this](std::string_view message) {
							m_notifications.Push(std::string(message));
						},
					});
				}
				break;
			}
			}
		}, ImGui::GetContentRegionAvail());

	if (m_showSelectionDialog)
	{
		if (!ImGui::IsPopupOpen("sample-selection"))
			ImGui::OpenPopup("sample-selection");
		const SelectionDialogModel model{
			.popupId = "sample-selection",
			.title = "Choose an item",
			.entries = {
				{ "one", "First item", "Available" },
				{ "two", "Second item", "Available" },
			},
		};
		const auto result = DrawSelectionDialog(model, context,
			CurrentWidgetTheme());
		if (result.selected || result.closed)
			m_showSelectionDialog = false;
	}
	if (m_showConfirmation)
	{
		const UnitRect confirmationPanel = CenteredPanel(context, 640.0f, 390.0f);
		DrawOverlayPanel(draw, context, confirmationPanel, CurrentWidgetTheme(), 1.0f);
		DrawOverlayHeader(draw, context, confirmationPanel, CurrentWidgetTheme(),
			"Confirmation");
		const auto result = DrawOverlayConfirmation(context, draw,
			confirmationPanel, {
				.prompt = "Apply the sample operation?",
				.details = { "This demonstrates the retained overlay confirmation." },
				.actions = { { "apply", "Apply" }, { "cancel", "Cancel" } },
				.selectedAction = m_confirmationSelection,
			});
		if (result.activatedAction)
		{
			m_confirmationSelection = *result.activatedAction;
			m_showConfirmation = false;
		}
	}
	if (m_showTextEditor)
	{
		const UnitRect editorPanel = CenteredPanel(context, 640.0f, 390.0f);
		DrawOverlayPanel(draw, context, editorPanel, CurrentWidgetTheme(), 1.0f);
		DrawOverlayHeader(draw, context, editorPanel, CurrentWidgetTheme(),
			"Text editor");
		const auto result = DrawTextEditorPanel(context, draw, editorPanel,
			m_textEditorState, m_editorBuffer.data(), m_editorBuffer.size());
		if (result.save)
		{
			std::strncpy(m_displayName.data(), m_editorBuffer.data(),
				m_displayName.size() - 1);
			m_displayName.back() = '\0';
			m_showTextEditor = false;
		}
		else if (result.cancel)
			m_showTextEditor = false;
	}
	if (m_showFormEditor)
	{
		const UnitRect formPanel = CenteredPanel(context, 760.0f, 560.0f);
		DrawOverlayPanel(draw, context, formPanel, CurrentWidgetTheme(), 1.0f);
		DrawOverlayHeader(draw, context, formPanel, CurrentWidgetTheme(),
			"Form editor");
		const std::array sections{
			FormSection{ "general", "General", {
				{ "enabled", "Enabled", {}, FormFieldKind::Toggle, m_featureEnabled },
				{ "preset", "Preset", "Balanced", FormFieldKind::Choice },
			} },
			FormSection{ "details", "Details", {
				{ "owner", "Owner", m_displayName.data(), FormFieldKind::ReadOnly },
			} },
		};
		const std::array choices{
			ChoiceItem{ "balanced", "Balanced" },
			ChoiceItem{ "quality", "Quality" },
			ChoiceItem{ "performance", "Performance" },
		};
		const auto result = DrawFormEditor(context, draw, formPanel,
			m_formEditorState, sections, choices);
		if (result.activatedField && m_formEditorState.section == 0 &&
			*result.activatedField == 0)
		{
			m_featureEnabled = !m_featureEnabled;
		}
		if (result.save || result.cancel)
			m_showFormEditor = false;
	}
	if (m_showQuickMenu)
	{
		const QuickMenuModel model{
			.windowId = "sample-quick-menu",
			.title = "Quick Menu",
			.rows = {
				{ "resume", "Return to application" },
				{ "layout", "Layout", "Primary only" },
				{ "restart", "Restart content" },
				{ "quit", "Quit content" },
			},
			.selectedRow = m_quickMenuRow,
			.choicePopupOpen = m_quickMenuChoiceOpen,
			.choiceTitle = "Layout",
			.choices = {
				{ "primary", "Primary only" },
				{ "secondary", "Secondary only" },
				{ "side", "Side by side" },
			},
			.selectedChoice = m_quickMenuChoice,
		};
		const auto result = DrawQuickMenu(model, CurrentWidgetTheme());
		if (result.activatedRow)
		{
			m_quickMenuRow = *result.activatedRow;
			if (*result.activatedRow == 0)
				m_showQuickMenu = false;
			else if (*result.activatedRow == 1)
				m_quickMenuChoiceOpen = true;
		}
		if (result.activatedChoice)
		{
			m_quickMenuChoice = *result.activatedChoice;
			m_quickMenuChoiceOpen = false;
		}
	}
	if (m_showProgressDialog)
	{
		if (!ImGui::IsPopupOpen("sample-progress"))
			ImGui::OpenPopup("sample-progress");
		if (BeginThemedModal("sample-progress", context, CurrentWidgetTheme(),
			{ 520.0f, 0.0f }))
		{
			SectionTitle("Background operation");
			DrawProgressDialogBody("Processing sample content", 0.68f,
				"The host supplies the operation and progress value.");
			if (ShellButton("Close", { 180.0f, 46.0f }))
			{
				m_showProgressDialog = false;
				ImGui::CloseCurrentPopup();
			}
			EndThemedModal();
		}
	}
}

void MinimalHost::DrawContextMenu(const ShellLayoutContext& context,
	ImDrawList* draw, const UnitRect& panel)
{
	if (!m_contextItem)
		return;
	const auto found = std::ranges::find_if(m_catalogue,
		[this](const LibraryItem& item) { return item.id == *m_contextItem; });
	if (found == m_catalogue.end())
	{
		CloseContextMenu();
		return;
	}

	std::array<char, 17> titleId{};
	const std::uint64_t contentId = found->contentId != 0 ? found->contentId : found->id;
	std::snprintf(titleId.data(), titleId.size(), "%016llX",
		static_cast<unsigned long long>(contentId));
	if (m_metadataEditor && m_metadataEditor->IsOpen())
	{
		DrawOverlayHeader(draw, context, panel, CurrentWidgetTheme(),
			m_metadataEditor->Heading(), NormalizeDisplayText(found->name),
			titleId.data(), ResolveFrontendTextFont());
		const auto event = m_metadataEditor->Draw(context, draw, panel,
			CurrentWidgetTheme(), ResolveFrontendTextFont());
		(void)event;
		return;
	}
	TitleOptionsModel model;
	model.artwork = m_contextArtwork;
	model.title = NormalizeDisplayText(found->name);
	model.titleId = titleId.data();
	model.subtitle = found->platform;
	model.metadata = found->metadata;
	model.information = {
		{ "Platform", found->platform },
		{ "Format", found->format },
		{ "Region", found->region },
		{ "Version", std::to_string(found->version) },
	};
	model.detailControllerScroll = std::exchange(m_contextDetailScroll, 0.0f);
	model.actions = MakeDefaultTitleOptionActions(m_favorites.contains(found->id));
	const TitleOptionsResult result = DrawTitleOptionsView(context, draw, panel,
		m_titleOptionsState, model, ResolveFrontendTextFont());
	if (result.activatedAction)
		ActivateTitleOption(*result.activatedAction);
	if (result.closeRequested)
		CloseContextMenu();
}

void MinimalHost::DrawInputMapper(const ShellLayoutContext& context,
	ImDrawList* draw)
{
	if (!m_inputMapper.IsOpen())
		OpenInputMapper(m_mapperScope == MapperScope::Title ? m_contextItem :
			std::optional<ItemId>{});
	const InputMapperResult result = m_inputMapper.Draw(context, draw,
		m_mapperFrameInput, ResolveFrontendTextFont());
	m_mapperFrameInput = {};
	if (!result.error.empty())
		m_notifications.Push(result.error);
	if (result.saved)
		m_notifications.Push(m_mapperScope == MapperScope::Title ?
			"Per-title controller profile saved" : "Global controller profiles saved");
	if (result.saved || result.cancelled || !m_inputMapper.IsOpen())
	{
		const MapperScope scope = std::exchange(m_mapperScope, MapperScope::None);
		m_inputMapper.Close();
		for (auto& repeat : m_mapperRepeat)
			repeat.Reset();
		if (scope == MapperScope::Global)
			m_activePage.clear();
	}
}

void MinimalHost::ActivateTitleOption(std::string_view action)
{
	if (!m_contextItem)
		return;
	if (action == "start")
	{
		(void)RequestLaunch(*m_contextItem);
		CloseContextMenu();
	}
	else if (action == "favorite")
	{
		if (!m_favorites.erase(*m_contextItem))
			m_favorites.insert(*m_contextItem);
	}
	else if (action == "metadata")
	{
		OpenMetadataEditor();
	}
	else if (action == "controller-mapping")
	{
		OpenInputMapper(m_contextItem);
	}
	else if (action == "refresh")
	{
		m_notifications.Push("Sample catalogue refreshed");
	}
}

void MinimalHost::CloseContextMenu()
{
	if (m_metadataEditor && m_metadataEditor->IsOpen())
		m_metadataEditor->Close();
	if (m_contextArtwork)
	{
		ReleaseTexture(m_contextArtwork);
		m_contextArtwork = {};
	}
	if (m_mapperScope == MapperScope::Title)
	{
		m_inputMapper.Close();
		m_mapperScope = MapperScope::None;
	}
	m_contextItem.reset();
	m_titleOptionsState = {};
}

void MinimalHost::OpenInputMapper(std::optional<ItemId> item)
{
	m_inputMapper.Close();
	m_mapperScope = item ? MapperScope::Title : MapperScope::Global;
	m_mapperFrameInput = {};
	for (auto& repeat : m_mapperRepeat)
		repeat.Reset();
	m_inputMapper.Open(m_inputMappingHost,
		item ? std::optional<std::uint64_t>(*item) : std::nullopt, 0);
}

void MinimalHost::OpenMetadataEditor()
{
	if (!m_contextItem || !m_metadataEditor)
		return;
	const auto found = std::find_if(m_catalogue.begin(), m_catalogue.end(),
		[this](const LibraryItem& item) { return item.id == *m_contextItem; });
	if (found == m_catalogue.end())
		return;
	const std::filesystem::path recordPath = StateRoot() / "metadata" /
		(std::to_string(found->id) + ".json");
	GameMetadataRecord record{
		.itemId = found->id,
		.title = found->name,
		.metadata = found->metadata,
	};
	if (const auto saved = LoadGameMetadataRecord(recordPath))
		record = *saved;
	ScreenScraperCredentials account;
	if (m_credentials)
		account = m_credentials->Load();
	ScreenScraperSearchRequest search{
		.account = std::move(account),
		.systemId = kScreenScraperPs3SystemId,
		.platformName = found->platform,
		.regionHint = found->region,
		.previewDirectory = StateRoot() / "metadata" / ".search",
	};
	m_metadataEditor->Open({
		.record = std::move(record),
		.defaultTitle = found->name,
	}, {
		.listArtwork = ScreenScraperArtwork::Box2D,
		.detailArtwork = ScreenScraperArtwork::FanArt,
		.searchRequest = std::move(search),
	}, {
		.loadTexture = [this](const std::filesystem::path& path) {
			return LoadTexture(path);
		},
		.save = [this, recordPath](const GameMetadataRecord& saved, bool) {
			const bool ok = SaveGameMetadataRecordAtomic(recordPath, saved);
			m_notifications.Push(ok ? "Metadata saved" : "Could not save metadata");
			return ok;
		},
		.notify = [this](std::string_view message) {
			m_notifications.Push(std::string(message));
		},
	});
}

MinimalFrontend::MinimalFrontend()
	: m_screenScraper(CreateScreenScraperService()),
	  m_companion(CreateCompanionService())
{
	FrontendConfiguration configuration = MakeDefaultFrontendConfiguration();
	configuration.applicationName = "RPCS3-UWP";
	configuration.platformName = "PlayStation 3";
	m_frontend = std::make_unique<Frontend>(m_host, *m_screenScraper, *m_companion,
		std::move(configuration));
	m_host.AttachFrontend(m_frontend.get());
	m_renderer = std::make_unique<ShellRenderer>(m_host);
}

MinimalFrontend::~MinimalFrontend()
{
	Shutdown();
}

void MinimalFrontend::Initialize()
{
	if (m_initialized)
		return;
	InitializeNativeTextInput({
		.contextName = "RPCS3-UWP",
		.log = [this](LogLevel level, std::string_view message) {
			m_host.Log(level, message);
		},
	});
	m_host.AttachScreenScraper(*m_screenScraper);
	m_frontend->Initialize();
	m_frontend->SetAnimateLaunches(m_host.InterfaceSettings().animateLaunches);
	m_initialized = true;
}

void MinimalFrontend::Shutdown()
{
	if (!m_initialized)
		return;
	m_frontend->Shutdown();
	m_host.AttachFrontend(nullptr);
	ShutdownNativeTextInput();
	m_initialized = false;
}

void MinimalFrontend::DrawFrame(const FrameInput& input,
	const ShellPresentation& presentation)
{
	const auto hasCommandInput = [](const FrameInput& value) {
		return value.up || value.down || value.left || value.right || value.accept ||
			value.back || value.context || value.alternate || value.menu || value.view ||
			value.leftShoulder || value.rightShoulder || value.pointerPressed ||
			std::abs(value.leftStickX) > 0.2f ||
			std::abs(value.leftStickY) > 0.2f ||
			std::abs(value.rightStickX) > 0.2f ||
			std::abs(value.rightStickY) > 0.2f;
	};
	BeginNativeTextInputFrame();
	const ShellTheme& theme = presentation.customTheme ? *presentation.customTheme :
		ShellThemes()[static_cast<std::size_t>(std::clamp(presentation.themeIndex,
			0, static_cast<int>(ShellThemes().size()) - 1))];
	BeginWidgetFrame(theme, {
		.accept = input.accept,
		.back = input.back,
		.up = input.up,
		.down = input.down,
		.left = input.left,
		.right = input.right,
		.leftShoulder = input.leftShoulder,
		.rightShoulder = input.rightShoulder,
	}, {
		.accept = m_previousInput.accept,
		.back = m_previousInput.back,
		.up = m_previousInput.up,
		.down = m_previousInput.down,
		.left = m_previousInput.left,
		.right = m_previousInput.right,
		.leftShoulder = m_previousInput.leftShoulder,
		.rightShoulder = m_previousInput.rightShoulder,
	}, m_host.OwnsOverlayInput());
	const bool overlayOwnedAtFrameStart = m_host.OwnsOverlayInput();
	// Desktop widgets, including the File--Help menu bar, own gamepad focus.
	// Do not let the retained console-shell navigation activate a game or open
	// settings while the same A/B/Menu press is being handled by ImGui.
	FrameInput shellInput = input;
	const bool menuOwnsInput = input.menu ||
		ImGui::GetCurrentContext()->NavLayer == ImGuiNavLayer_Menu ||
		ImGui::GetCurrentContext()->NavWindowingTarget != nullptr ||
		ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
	if (!overlayOwnedAtFrameStart && (ImGui::GetIO().NavActive || input.menu ||
		ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)))
	{
		shellInput = {};
		shellInput.deltaSeconds = input.deltaSeconds;
		if (!menuOwnsInput)
		{
			shellInput.context = input.context;
			shellInput.view = input.view;
			shellInput.alternate = input.alternate;
		}
	}
	if (!m_host.HandleOverlayInput(input, m_previousInput) &&
		!m_shellInputReleaseBarrier)
	{
		m_frontend->HandleInput(shellInput);
	}
	m_renderer->Draw(*m_frontend, presentation, input.deltaSeconds);
	m_host.DrawOverlayPages();
	const bool overlayOwnedAtFrameEnd = m_host.OwnsOverlayInput();
	if (overlayOwnedAtFrameStart && !overlayOwnedAtFrameEnd)
		m_shellInputReleaseBarrier = true;
	if (!overlayOwnedAtFrameEnd && m_shellInputReleaseBarrier &&
		!hasCommandInput(input))
	{
		// Resume shell input after the closing command is released.
		m_frontend->HandleInput(input);
		m_shellInputReleaseBarrier = false;
	}
	EndWidgetFrame();
	EndNativeTextInputFrame();
	m_previousInput = input;
}
}
