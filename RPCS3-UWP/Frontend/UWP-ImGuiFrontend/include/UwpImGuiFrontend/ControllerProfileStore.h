#pragma once

#include "InputMapping.h"

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace UwpImGuiFrontend
{
struct ControllerProfileStoreSnapshot
{
	std::vector<ControllerProfileDraft> activeProfiles;
	std::vector<ControllerProfileDraft> namedProfiles;
};

[[nodiscard]] bool LoadControllerProfileStore(
	const std::filesystem::path& path, ControllerProfileStoreSnapshot& snapshot,
	std::string* error = nullptr);
[[nodiscard]] bool SaveControllerProfileStoreAtomic(
	const std::filesystem::path& path,
	const ControllerProfileStoreSnapshot& snapshot,
	std::string* error = nullptr);

enum class DsuInputFeature : std::uint8_t
{
	Gamepad,
	Motion,
	Touch,
};

class DsuRouteTable
{
public:
	[[nodiscard]] MappingSaveResult Apply(
		std::span<const ControllerProfileDraft> profiles);
	void Clear() noexcept;

	[[nodiscard]] const DsuPlayerRoute* RouteForPlayer(
		std::uint8_t player) const noexcept;
	[[nodiscard]] bool Accepts(std::uint8_t player, std::string_view serverId,
		std::uint8_t remoteSlot, DsuInputFeature feature) const noexcept;
	[[nodiscard]] bool UsesAttachedDevices(std::uint8_t player) const noexcept;

private:
	std::array<std::optional<DsuPlayerRoute>, 8> m_routes;
};
}
