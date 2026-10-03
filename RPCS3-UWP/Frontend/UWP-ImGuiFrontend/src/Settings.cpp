#include "UwpImGuiFrontend/Settings.h"

#include "UwpImGuiFrontend/ImGuiShell.h"
#include "UwpImGuiFrontend/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <array>

namespace UwpImGuiFrontend
{
namespace
{
constexpr std::string_view kInterfacePage = "interface";
constexpr std::string_view kScreenScraperPage = "screenscraper";
constexpr std::string_view kCompanionPage = "dsu-streaming";
constexpr std::string_view kStoragePage = "storage";

void AddTab(std::vector<SplitViewTab>& tabs, std::string_view id,
	std::string_view label, bool visible)
{
	if (visible)
		tabs.push_back({ std::string(id), std::string(label), true });
}

void DrawUnavailable(std::string_view message)
{
	SectionTitle("Unavailable");
	ImGui::TextWrapped("%.*s", static_cast<int>(message.size()), message.data());
}
}

std::vector<SplitViewTab> BuildSettingsTabs(const SettingsViewModel& model,
	std::span<const SettingsExtensionPage> extensions)
{
	std::vector<SplitViewTab> tabs;
	tabs.reserve(4 + extensions.size());
	AddTab(tabs, kInterfacePage, "Interface", model.showInterface);
	AddTab(tabs, kCompanionPage, "DSU/Streaming", model.showCompanion);
	AddTab(tabs, kStoragePage, "Storage", model.showStorage);
	AddTab(tabs, kScreenScraperPage, "ScreenScraper", model.showScreenScraper);
	for (const SettingsExtensionPage& extension : extensions)
	{
		if (!extension.id.empty())
			tabs.push_back({ extension.id, extension.label, extension.enabled });
	}
	return tabs;
}

void SettingsView::Draw(SettingsViewModel& model,
	const SettingsViewServices& services, const SettingsViewCallbacks& callbacks,
	std::span<const SettingsExtensionPage> extensions, ImVec2 size,
	const SplitViewOptions& options)
{
	const std::vector<SplitViewTab> tabs = BuildSettingsTabs(model, extensions);
	if (tabs.empty())
	{
		ImGui::TextDisabled("No settings pages are available.");
		m_selectedPageId.clear();
		return;
	}

	if (!m_selectedPageId.empty())
	{
		const auto selected = std::find_if(tabs.begin(), tabs.end(), [this](const auto& tab) {
			return tab.id == m_selectedPageId;
		});
		if (selected != tabs.end())
			m_state.Select(static_cast<std::size_t>(selected - tabs.begin()), tabs.size());
	}
	m_state.SetItemCount(tabs.size());
	if (!tabs[m_state.selected].enabled)
	{
		const auto enabled = std::find_if(tabs.begin(), tabs.end(),
			[](const auto& tab) { return tab.enabled; });
		if (enabled == tabs.end())
		{
			ImGui::TextDisabled("No settings pages are currently enabled.");
			m_selectedPageId.clear();
			return;
		}
		m_state.Select(static_cast<std::size_t>(enabled - tabs.begin()), tabs.size());
	}
	m_selectedPageId = tabs[m_state.selected].id;

	DrawSplitView("reusable-settings", m_state, tabs,
		[&](std::size_t index) {
			if (index >= tabs.size())
				return;
			m_selectedPageId = tabs[index].id;
			const std::string_view id = tabs[index].id;
			if (id == kInterfacePage)
				DrawInterface(model.interfaceSettings, callbacks);
			else if (id == kScreenScraperPage)
				DrawScreenScraper(model.screenScraper, services, callbacks);
			else if (id == kCompanionPage)
				DrawCompanion(model.companion, callbacks);
			else if (id == kStoragePage)
				DrawStorage(model.storage, services, callbacks);
			else
			{
				const auto extension = std::find_if(extensions.begin(), extensions.end(),
					[id](const auto& page) { return page.id == id; });
				if (extension != extensions.end() && extension->draw)
					extension->draw();
			}
		}, size, options);
}

void SettingsView::SelectPage(std::string_view id,
	const SettingsViewModel& model,
	std::span<const SettingsExtensionPage> extensions)
{
	const std::vector<SplitViewTab> tabs = BuildSettingsTabs(model, extensions);
	const auto selected = std::find_if(tabs.begin(), tabs.end(),
		[id](const auto& tab) { return tab.id == id && tab.enabled; });
	if (selected == tabs.end())
		return;
	m_state.Select(static_cast<std::size_t>(selected - tabs.begin()), tabs.size());
	m_state.RequestFocus(SplitPane::Navigation);
	m_selectedPageId = selected->id;
}

std::string_view SettingsView::SelectedPageId() const noexcept
{
	return m_selectedPageId;
}

void SettingsView::DrawInterface(InterfaceSettingsModel& model,
	const SettingsViewCallbacks& callbacks)
{
	bool changed = false;
	SectionTitle("Appearance");
	const auto& themes = ShellThemes();
	std::vector<ChoiceItem> choices;
	choices.reserve(themes.size());
	for (std::size_t index = 0; index < themes.size(); ++index)
		choices.push_back({ std::to_string(index), themes[index].name, true });
	std::size_t theme = static_cast<std::size_t>(std::clamp(model.themeIndex, 0,
		static_cast<int>(themes.size()) - 1));
	if (ShellCombo("Theme", &theme, choices))
	{
		model.themeIndex = static_cast<int>(theme);
		changed = true;
	}
	changed |= ShellCheckbox("Show controller action hints", &model.showActionHints);
	changed |= ShellCheckbox("Animate launches", &model.animateLaunches);
	if (changed && callbacks.persistInterface)
		callbacks.persistInterface(model);
}

void SettingsView::DrawScreenScraper(ScreenScraperPanelModel& model,
	const SettingsViewServices& services, const SettingsViewCallbacks& callbacks)
{
	if (!services.screenScraper || !services.credentials)
	{
		DrawUnavailable("Provide both IScreenScraperService and ICredentialStore to use this page.");
		return;
	}
	m_screenScraperPanel.Draw(model, *services.screenScraper,
		*services.credentials, callbacks.screenScraper, services.picker);
}

void SettingsView::DrawCompanion(CompanionPanelModel& model,
	const SettingsViewCallbacks& callbacks)
{
	m_companionPanel.Draw(model, callbacks.persistCompanion);
	if (callbacks.drawCompanionExtras)
		callbacks.drawCompanionExtras();
}

void SettingsView::DrawStorage(std::vector<StorageSettingsEntry>& entries,
	const SettingsViewServices& services,
	const SettingsViewCallbacks& callbacks)
{
	if (!services.picker)
	{
		DrawUnavailable("Provide IPickerService to use controller-friendly storage locations.");
		if (callbacks.drawStorageExtras)
			callbacks.drawStorageExtras();
		return;
	}
	if (entries.empty())
	{
		SectionTitle("Storage locations");
		ImGui::TextDisabled("No storage locations were supplied by the host.");
		return;
	}
	for (std::size_t index = 0; index < entries.size(); ++index)
	{
		StorageSettingsEntry& entry = entries[index];
		const std::string sectionLabel = entry.model.label.empty() ?
			"Storage location" : std::string(entry.model.label);
		SectionTitle(sectionLabel.c_str());
		ImGui::PushID(static_cast<int>(index));
		(void)DrawStorageLocationSelector(entry.model, *services.picker,
			entry.callbacks);
		ImGui::PopID();
	}
	if (callbacks.drawStorageExtras)
		callbacks.drawStorageExtras();
}

}
