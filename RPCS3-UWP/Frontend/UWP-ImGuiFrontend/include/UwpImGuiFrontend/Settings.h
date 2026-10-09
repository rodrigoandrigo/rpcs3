#pragma once

#include "ServicePanels.h"
#include "StorageLocations.h"
#include "Views.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace UwpImGuiFrontend
{
struct InterfaceSettingsModel
{
	int themeIndex = 0;
	bool showActionHints = true;
	bool animateLaunches = true;
};

struct StorageSettingsEntry
{
	StorageLocationSelectorModel model;
	StorageLocationSelectorCallbacks callbacks;
};

struct SettingsExtensionPage
{
	std::string id;
	std::string label;
	bool enabled = true;
	std::function<void()> draw;
};

struct SettingsViewModel
{
	InterfaceSettingsModel interfaceSettings;
	ScreenScraperPanelModel screenScraper;
	CompanionPanelModel companion;
	std::vector<StorageSettingsEntry> storage;
	bool showInterface = true;
	bool showScreenScraper = true;
	bool showCompanion = true;
	bool showStorage = true;
};

struct SettingsViewServices
{
	IScreenScraperService* screenScraper = nullptr;
	ICredentialStore* credentials = nullptr;
	IPickerService* picker = nullptr;
};

struct SettingsViewCallbacks
{
	std::function<void(const InterfaceSettingsModel&)> persistInterface;
	ScreenScraperPanelCallbacks screenScraper;
	PersistCompanionPanel persistCompanion;
	// Optional host-specific controls appended to the portable built-in pages.
	std::function<void()> drawCompanionExtras;
	std::function<void()> drawStorageExtras;
};

[[nodiscard]] std::vector<SplitViewTab> BuildSettingsTabs(
	const SettingsViewModel& model,
	std::span<const SettingsExtensionPage> extensions = {});

class SettingsView
{
public:
	void Draw(SettingsViewModel& model, const SettingsViewServices& services,
		const SettingsViewCallbacks& callbacks,
		std::span<const SettingsExtensionPage> extensions = {},
		ImVec2 size = ImVec2(0.0f, 0.0f),
		const SplitViewOptions& options = {});

	void SelectPage(std::string_view id, const SettingsViewModel& model,
		std::span<const SettingsExtensionPage> extensions = {});
	[[nodiscard]] std::string_view SelectedPageId() const noexcept;
	[[nodiscard]] SplitViewState& State() noexcept { return m_state; }
	[[nodiscard]] const SplitViewState& State() const noexcept { return m_state; }

private:
	void DrawInterface(InterfaceSettingsModel& model,
		const SettingsViewCallbacks& callbacks);
	void DrawScreenScraper(ScreenScraperPanelModel& model,
		const SettingsViewServices& services,
		const SettingsViewCallbacks& callbacks);
	void DrawCompanion(CompanionPanelModel& model,
		const SettingsViewCallbacks& callbacks);
	void DrawStorage(std::vector<StorageSettingsEntry>& entries,
		const SettingsViewServices& services,
		const SettingsViewCallbacks& callbacks);
	SplitViewState m_state;
	ScreenScraperPanel m_screenScraperPanel;
	CompanionPanel m_companionPanel;
	std::string m_selectedPageId;
	bool m_selectRequested = false;
};
}
