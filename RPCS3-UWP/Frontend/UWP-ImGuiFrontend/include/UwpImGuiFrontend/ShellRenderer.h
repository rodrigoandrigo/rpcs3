#pragma once

#include "Frontend.h"
#include "ImGuiShell.h"

#include <memory>
#include <optional>

namespace UwpImGuiFrontend
{
struct ShellPresentation
{
	int themeIndex = 0;
	const ShellTheme* customTheme = nullptr;
	ImFont* font = nullptr;
	bool showActionHints = false;
	std::string controllerStatus;
	// Hosts opt in when battery telemetry is available.
	bool showControllerBattery = false;
	float controllerBatteryLevel = 0.0f;
	std::vector<MediaAsset> platformMedia;
	MediaKind listArtwork = MediaKind::Cover3D;
	// Optional catalogue item used for dynamic background media.
	std::optional<ItemId> backgroundItem;
	std::optional<MediaKind> backgroundArtwork;
	bool videoBackground = false;
	bool allowPlatformBackgroundFallback = true;
	// Treats backgroundArtwork and videoBackground as authoritative.
	bool overrideThemeDynamicBackground = false;
};

class ShellRenderer
{
public:
	explicit ShellRenderer(IFrontendHost& host);
	~ShellRenderer();

	ShellRenderer(const ShellRenderer&) = delete;
	ShellRenderer& operator=(const ShellRenderer&) = delete;
	ShellRenderer(ShellRenderer&&) noexcept;
	ShellRenderer& operator=(ShellRenderer&&) noexcept;

	void Draw(Frontend& frontend, const ShellPresentation& presentation,
		float deltaSeconds);
	void ClearTextures();

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};
}
