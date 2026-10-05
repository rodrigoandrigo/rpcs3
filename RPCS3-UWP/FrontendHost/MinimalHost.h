#pragma once

#include "SampleInputMappingHost.h"
#include "UwpImGuiFrontend/UwpImGuiFrontend.h"

#include <array>
#include <chrono>
#include <atomic>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace UwpImGuiFrontend::Sample
{
class MinimalHost final : public IFrontendHost
{
public:
	MinimalHost();
	~MinimalHost() override;

	std::vector<LibraryItem> SnapshotCatalogue() override;
	RunningContent GetRunningContent() const override;
	DsuEndpoint GetDsuEndpoint() const override;
	bool StartStream(const StreamRequest& request) override;
	void StopStream(bool secondaryScreen) override;
	std::optional<ActiveStream> GetActiveStream() const override;
	std::optional<std::string> TakeStreamFailure() override;
	void Log(LogLevel level, std::string_view message) override;

	std::filesystem::path ResourceRoot() const override;
	std::filesystem::path StateRoot() const override;
	TextureHandle LoadTexture(const std::filesystem::path& path) override;
	void ReleaseTexture(TextureHandle texture) override;
	bool RequestLaunch(ItemId item) override;
	void OpenContextMenu(ItemId item) override;
	void Execute(HostCommand command) override;
	void DrawSettingsPage(std::string_view pageId) override;
	void OpenUri(std::string_view uri) override;
	void PersistFrontendState(std::string_view data) override;
	std::string LoadFrontendState() const override;

	void SetCatalogue(std::vector<LibraryItem> catalogue);
#ifdef RPCS3_HOST_WITH_CORE
	void OnCoreEvent(std::uint32_t type, std::uint32_t command, std::int32_t result,
		std::string_view message);
#endif
	void SetDsuEndpoint(DsuEndpoint endpoint);
	void AttachScreenScraper(IScreenScraperService& service);
	void SetTextureCallbacks(
		std::function<TextureHandle(const std::filesystem::path&)> load,
		std::function<void(TextureHandle)> release);
	void SetInputCallbacks(SampleInputMappingHost::DeviceEnumerator enumerate,
		SampleInputMappingHost::LiveInputReader read);
	[[nodiscard]] const InterfaceSettingsModel& InterfaceSettings() const noexcept
	{
		return m_interfaceSettings;
	}
	void AttachFrontend(Frontend* frontend) noexcept { m_frontend = frontend; }
	[[nodiscard]] bool OwnsOverlayInput() const noexcept
	{
		return !m_activePage.empty() || m_contextItem.has_value();
	}
	bool HandleOverlayInput(const FrameInput& input, const FrameInput& previousInput);
	void DrawOverlayPages();

private:
	void PersistInterfaceSettings();
#ifdef RPCS3_HOST_WITH_CORE
	struct CoreSetting
	{
		std::uint32_t type = 0;
		std::uint32_t flags = 0;
		std::string path;
		std::string name;
		std::string value;
		std::string defaultValue;
		std::string group;
		std::string minimumValue;
		std::string maximumValue;
		std::string restriction;
		std::vector<std::string> choices;
	};

	void PickGameFolder();
	winrt::fire_and_forget LoadGameFolder(std::wstring token);
	void PickPackage();
	winrt::fire_and_forget InstallPickedPackage(std::wstring token);
	void PickFirmware();
	winrt::fire_and_forget InstallPickedFirmware(std::wstring token);
	void OpenStateFolder(std::wstring_view relativePath);
	void RefreshCoreSettings();
	void DrawCoreSettingsPage(std::string_view group);
	void SetCoreSetting(const CoreSetting& setting, std::string value);
	std::vector<CoreSetting> m_coreSettings;
	std::unordered_map<std::string, std::vector<char>> m_coreSettingEditors;
	std::array<char, 256> m_coreSettingsFilter{};
	bool m_coreReady = false;
	bool m_libraryLoading = false;
	std::string m_gameMount;
	std::string m_packageMount;
	std::string m_firmwareMount;
	std::uint64_t m_mountGeneration = 0;
	std::shared_ptr<std::atomic_bool> m_alive = std::make_shared<std::atomic_bool>(true);
#endif
	void DrawComponentGallery(const ShellLayoutContext& context,
		ImDrawList* draw);
	void DrawGraphicsPage();
	void DrawToolsPage();
	void DrawContextMenu(const ShellLayoutContext& context,
		ImDrawList* draw, const UnitRect& panel);
	void DrawInputMapper(const ShellLayoutContext& context, ImDrawList* draw);
	void ActivateTitleOption(std::string_view action);
	void CloseContextMenu();
	void OpenMetadataEditor();
	void OpenInputMapper(std::optional<ItemId> item);

	struct DirectionRepeat
	{
		bool Update(bool held, float deltaSeconds) noexcept;
		void Reset() noexcept;

		bool wasHeld = false;
		float elapsed = 0.0f;
		float nextRepeat = 0.30f;
	};

	enum class MapperScope
	{
		None,
		Global,
		Title,
	};

	std::vector<LibraryItem> m_catalogue;
	std::function<TextureHandle(const std::filesystem::path&)> m_loadTexture;
	std::function<void(TextureHandle)> m_releaseTexture;
	DsuEndpoint m_endpoint;
	RunningContent m_runningContent;
	std::chrono::steady_clock::time_point m_gameStarted{};
	std::string m_playingSerial;
	void SavePlayHistory();
#ifdef RPCS3_HOST_WITH_CORE
	std::optional<RunningContent> m_pendingCoreLaunch;
#endif
	std::optional<std::string> m_streamFailure;
	std::filesystem::path m_resourceRoot;
	std::filesystem::path m_stateRoot;
	winrt::Windows::UI::Core::CoreDispatcher m_uiDispatcher{nullptr};
	std::filesystem::path m_storagePath{ L"E:\\SampleFrontend" };
	std::string m_activePage;
	std::optional<ItemId> m_contextItem;
	IScreenScraperService* m_screenScraperService = nullptr;
	Frontend* m_frontend = nullptr;
	std::unique_ptr<MetadataEditor> m_metadataEditor;
	std::unique_ptr<ICredentialStore> m_credentials;
	std::unique_ptr<IPickerService> m_picker;
	ScreenScraperPanel m_screenScraperPanel;
	ScreenScraperPanelModel m_screenScraperModel;
	CompanionPanel m_companionPanel;
	CompanionPanelModel m_companionModel;
	SplitViewState m_splitState;
	DataTableState m_tableState;
	ActionListState m_actionListState;
	NotificationCenter m_notifications;
	SettingsView m_settingsView;
	LibraryView m_libraryView;
	GraphicsView m_graphicsView;
	SampleInputMappingHost m_inputMappingHost;
	InputMapperView m_inputMapper;
	MapperScope m_mapperScope = MapperScope::None;
	FrameInput m_mapperFrameInput;
	std::array<DirectionRepeat, 4> m_mapperRepeat;
	TitleOptionsState m_titleOptionsState;
	TextureHandle m_contextArtwork;
	float m_contextDetailScroll = 0.0f;
	std::unordered_set<ItemId> m_favorites;
	InterfaceSettingsModel m_interfaceSettings;
	std::string m_pendingSettingsPage;
	bool m_libraryContextRequested = false;
	std::array<char, 128> m_displayName{ "Sample user" };
	std::filesystem::path m_gamesPath{ L"E:\\Games" };
	bool m_featureEnabled = true;
	float m_interfaceScale = 1.0f;
	int m_workerCount = 4;
	int m_cacheLimit = 512;
	std::size_t m_quality = 1;
	ImVec4 m_accent{ 0.13f, 0.72f, 0.92f, 1.0f };
	bool m_showSelectionDialog = false;
	bool m_showConfirmation = false;
	bool m_showTextEditor = false;
	bool m_showQuickMenu = false;
	bool m_showFormEditor = false;
	bool m_showProgressDialog = false;
	TextEditorPanelState m_textEditorState;
	FormEditorState m_formEditorState;
	std::array<char, 128> m_editorBuffer{};
	std::size_t m_confirmationSelection = 1;
	std::size_t m_quickMenuRow = 0;
	std::size_t m_quickMenuChoice = 0;
	bool m_quickMenuChoiceOpen = false;
};

class MinimalFrontend
{
public:
	MinimalFrontend();
	~MinimalFrontend();

	void Initialize();
	void Shutdown();
	void DrawFrame(const FrameInput& input, const ShellPresentation& presentation);
	MinimalHost& Host() noexcept { return m_host; }

private:
	MinimalHost m_host;
	std::unique_ptr<IScreenScraperService> m_screenScraper;
	std::unique_ptr<ICompanionService> m_companion;
	std::unique_ptr<Frontend> m_frontend;
	std::unique_ptr<ShellRenderer> m_renderer;
	FrameInput m_previousInput;
	bool m_shellInputReleaseBarrier = false;
	bool m_initialized = false;
};
}
