#include "SampleInputMappingHost.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <set>
#include <utility>

namespace UwpImGuiFrontend::Sample
{
namespace
{
InputSettingDraft NumberSetting(std::string id, std::string label,
	std::string group, float value, float minimum, float maximum, float step,
	std::string suffix = {}, bool enabled = true)
{
	return { .id = std::move(id), .label = std::move(label),
		.group = std::move(group), .kind = InputSettingKind::Number,
		.enabled = enabled, .numberValue = value, .minimum = minimum,
		.maximum = maximum, .step = step, .suffix = std::move(suffix) };
}

InputSettingDraft ToggleSetting(std::string id, std::string label,
	std::string group, bool value, bool enabled = true)
{
	return { .id = std::move(id), .label = std::move(label),
		.group = std::move(group), .kind = InputSettingKind::Toggle,
		.enabled = enabled, .boolValue = value };
}

std::string InputReference(std::string_view device, std::string_view control)
{
	return EscapeInputControlReference(std::string(device) + "/" +
		std::string(control));
}

std::string TitleProfileName(std::uint64_t titleId, std::uint8_t player)
{
	std::array<char, 48> text{};
	std::snprintf(text.data(), text.size(), "title-%016llx-p%u",
		static_cast<unsigned long long>(titleId),
		static_cast<unsigned int>(player + 1));
	return text.data();
}

void AddCommonControls(std::vector<EmulatedControlDescriptor>& controls,
	bool sticks)
{
	for (const auto& [id, label] : std::array{
		std::pair{ "dpad_up", "Up" }, std::pair{ "dpad_right", "Right" },
		std::pair{ "dpad_down", "Down" }, std::pair{ "dpad_left", "Left" } })
	{
		controls.push_back({ id, label, "D-Pad", InputControlKind::Button });
	}
	if (!sticks)
		return;
	for (const auto& [id, label, group] : std::array{
		std::tuple{ "left_stick_click", "Click", "Left Stick" },
		std::tuple{ "left_stick_up", "Up", "Left Stick" },
		std::tuple{ "left_stick_right", "Right", "Left Stick" },
		std::tuple{ "left_stick_down", "Down", "Left Stick" },
		std::tuple{ "left_stick_left", "Left", "Left Stick" },
		std::tuple{ "right_stick_click", "Click", "Right Stick" },
		std::tuple{ "right_stick_up", "Up", "Right Stick" },
		std::tuple{ "right_stick_right", "Right", "Right Stick" },
		std::tuple{ "right_stick_down", "Down", "Right Stick" },
		std::tuple{ "right_stick_left", "Left", "Right Stick" } })
	{
		controls.push_back({ id, label, group, InputControlKind::Axis });
	}
}


std::string_view ControllerPreviewAsset(std::string_view controllerTypeId)
{
	return controllerTypeId == "xbox-one-series" ? "xbox-one-series.png" : std::string_view{};
}
}

SampleInputMappingHost::~SampleInputMappingHost()
{
	if (m_releaseTexture)
	{
		for (const auto& [_, texture] : m_controllerPreviewTextures)
		{
			if (texture)
				m_releaseTexture(texture);
		}
	}
}

void SampleInputMappingHost::SetInputCallbacks(DeviceEnumerator enumerate,
	LiveInputReader read)
{
	m_enumerateDevices = std::move(enumerate);
	m_readInputs = std::move(read);
}

void SampleInputMappingHost::SetDsuServerEnumerator(DsuServerEnumerator enumerate)
{
	m_enumerateDsuServers = std::move(enumerate);
}

void SampleInputMappingHost::SetControllerPreviewCallbacks(
	std::filesystem::path assetRoot, TextureLoader load, TextureReleaser release)
{
	if (m_releaseTexture)
	{
		for (const auto& [_, texture] : m_controllerPreviewTextures)
		{
			if (texture)
				m_releaseTexture(texture);
		}
	}
	m_controllerPreviewTextures.clear();
	m_controllerPreviewRoot = std::move(assetRoot);
	m_loadTexture = std::move(load);
	m_releaseTexture = std::move(release);
}

MappingSaveResult SampleInputMappingHost::SetProfileStorePath(
	std::filesystem::path path)
{
	ControllerProfileStoreSnapshot snapshot;
	std::string error;
	if (!LoadControllerProfileStore(path, snapshot, &error))
		return { false, std::move(error) };

	std::unordered_map<std::string, ControllerProfileDraft> savedProfiles;
	std::array<std::unordered_map<std::string, ControllerProfileDraft>, 8>
		namedProfiles;
	for (ControllerProfileDraft& draft : snapshot.activeProfiles)
	{
		if (draft.player >= 8)
			return { false, "The controller profile store contains an invalid player" };
		if (draft.emulatedControllerTypeId != "xbox-one-series")
		{
			const auto name = draft.name;
			draft = MakeDefaultProfile(draft.player, draft.titleId, "xbox-one-series", draft.defaultDeviceId);
			draft.name = name;
		}
		draft.dirty = false;
		savedProfiles[ScopeKey(draft.player, draft.titleId)] = std::move(draft);
	}
	for (ControllerProfileDraft& draft : snapshot.namedProfiles)
	{
		if (draft.player >= 8 || draft.titleId || draft.name.empty())
			return { false, "The controller profile store contains an invalid named profile" };
		if (draft.emulatedControllerTypeId != "xbox-one-series")
		{
			const auto name = draft.name;
			draft = MakeDefaultProfile(draft.player, draft.titleId, "xbox-one-series", draft.defaultDeviceId);
			draft.name = name;
		}
		draft.dirty = false;
		const std::uint8_t player = draft.player;
		const std::string name = draft.name;
		namedProfiles[player][name] = std::move(draft);
	}
	DsuRouteTable routes;
	std::vector<ControllerProfileDraft> active;
	active.reserve(savedProfiles.size());
	for (const auto& [_, draft] : savedProfiles)
	{
		if (!draft.titleId)
			active.push_back(draft);
	}
	if (const MappingSaveResult applied = routes.Apply(active); !applied.succeeded)
		return applied;

	m_profileStorePath = std::move(path);
	m_savedProfiles = std::move(savedProfiles);
	m_namedProfiles = std::move(namedProfiles);
	m_activeTitleId.reset();
	m_appliedDsuRoutes = std::move(routes);
	return { true, {} };
}

const DsuPlayerRoute* SampleInputMappingHost::AppliedDsuRoute(
	std::uint8_t player) const noexcept
{
	return m_appliedDsuRoutes.RouteForPlayer(player);
}

bool SampleInputMappingHost::AcceptsDsuInput(std::uint8_t player,
	std::string_view serverId, std::uint8_t remoteSlot,
	DsuInputFeature feature) const
{
	const auto servers = m_enumerateDsuServers ? m_enumerateDsuServers() :
		std::vector<DsuServerModel>{};
	const auto server = std::ranges::find_if(servers,
		[serverId](const DsuServerModel& value) {
			return value.id == serverId && value.enabled;
		});
	return server != servers.end() &&
		m_appliedDsuRoutes.Accepts(player, serverId, remoteSlot, feature);
}

MappingSaveResult SampleInputMappingHost::ActivateProfileRoutes(
	std::optional<std::uint64_t> titleId)
{
	DsuRouteTable routes;
	if (const MappingSaveResult applied = BuildAppliedRoutes(titleId, routes);
		!applied.succeeded)
	{
		return applied;
	}
	m_activeTitleId = titleId;
	m_appliedDsuRoutes = std::move(routes);
	return { true, {} };
}

std::vector<InputDeviceDescriptor> SampleInputMappingHost::EnumerateInputDevices()
{
	return m_enumerateDevices ? m_enumerateDevices() :
		std::vector<InputDeviceDescriptor>{};
}

std::vector<DsuServerModel> SampleInputMappingHost::EnumerateDsuServers()
{
	return m_enumerateDsuServers ? m_enumerateDsuServers() :
		std::vector<DsuServerModel>{};
}

std::vector<EmulatedControllerDescriptor>
SampleInputMappingHost::EnumerateEmulatedControllerTypes(std::uint8_t)
{
	return {MakeXboxOneSeriesControllerDescriptor()};
}

TextureHandle SampleInputMappingHost::GetControllerPreviewTexture(
	std::string_view controllerTypeId)
{
	const std::string_view asset = ControllerPreviewAsset(controllerTypeId);
	if (asset.empty() || !m_loadTexture)
		return {};
	const std::string key(controllerTypeId);
	if (const auto loaded = m_controllerPreviewTextures.find(key);
		loaded != m_controllerPreviewTextures.end())
	{
		return loaded->second;
	}
	const TextureHandle texture =
		m_loadTexture(m_controllerPreviewRoot / std::string(asset));
	if (texture)
		m_controllerPreviewTextures.emplace(key, texture);
	return texture;
}

std::vector<EmulatedControlDescriptor> SampleInputMappingHost::ControlsFor(
	std::string_view controllerTypeId)
{
	if (controllerTypeId != "xbox-one-series") return {};
	std::vector<EmulatedControlDescriptor> controls;
	AddCommonControls(controls, true);
		for (const auto& [id, label, kind, group] : std::array{
			std::tuple{ "a", "A", InputControlKind::Button, "Buttons" },
			std::tuple{ "b", "B", InputControlKind::Button, "Buttons" },
			std::tuple{ "x", "X", InputControlKind::Button, "Buttons" },
			std::tuple{ "y", "Y", InputControlKind::Button, "Buttons" },
			std::tuple{ "l", "LB", InputControlKind::Button, "Buttons" },
			std::tuple{ "r", "RB", InputControlKind::Button, "Buttons" },
			std::tuple{ "zl", "LT", InputControlKind::Trigger, "Triggers" },
			std::tuple{ "zr", "RT", InputControlKind::Trigger, "Triggers" },
			std::tuple{ "plus", "Menu", InputControlKind::Button, "Buttons" },
			std::tuple{ "minus", "View", InputControlKind::Button, "Buttons" },
			std::tuple{ "home", "Xbox", InputControlKind::Button, "Buttons" } })
		{
			controls.push_back({ id, label, group, kind });
		}

	return controls;
}

std::vector<EmulatedControlDescriptor>
SampleInputMappingHost::EnumerateEmulatedControls(std::uint8_t,
	std::string_view controllerTypeId)
{
	return ControlsFor(controllerTypeId);
}

std::vector<InputSettingDraft> SampleInputMappingHost::CreateControllerSettings(
	std::uint8_t, std::string_view)
{
	return {};
}

InputDeviceSettingsDraft SampleInputMappingHost::CreateInputDeviceSettings(
	std::string_view deviceId)
{
	const auto devices = EnumerateInputDevices();
	const auto device = std::ranges::find_if(devices,
		[deviceId](const InputDeviceDescriptor& value) { return value.id == deviceId; });
	const bool available = device != devices.end();
	const bool rumble = available && device->supportsRumble;
	const bool motion = available && device->supportsMotion;
	const bool pointer = available && device->supportsPointer;
	return { std::string(deviceId), {
		ToggleSetting("motion", "Use motion", "General", true, motion),
		NumberSetting("rumble", "Rumble strength", "General", 50, 0, 100, 5, "%", rumble),
		NumberSetting("axis_deadzone", "Left stick deadzone", "Left Stick", 15, 0, 100, 1, "%", available),
		NumberSetting("axis_range", "Left stick range", "Left Stick", 100, 50, 200, 5, "%", available),
		NumberSetting("rotation_deadzone", "Right stick deadzone", "Right Stick", 15, 0, 100, 1, "%", available),
		NumberSetting("rotation_range", "Right stick range", "Right Stick", 100, 50, 200, 5, "%", available),
		NumberSetting("trigger_deadzone", "Trigger deadzone", "Triggers", 15, 0, 100, 1, "%", available),
		NumberSetting("trigger_range", "Trigger range", "Triggers", 100, 50, 200, 5, "%", available),
		ToggleSetting("pointer_enabled", "Virtual touch / IR pointer", "Pointer", true, pointer),
		NumberSetting("pointer_speed", "Pointer speed", "Pointer", 0.65f, 0.15f, 2.5f, 0.05f, {}, pointer),
		NumberSetting("pointer_deadzone", "Pointer stick dead zone", "Pointer", 18, 0, 95, 1, "%", pointer),
	} };
}

std::vector<std::string> SampleInputMappingHost::EnumerateProfileNames(
	std::uint8_t player)
{
	player = std::min<std::uint8_t>(player, 7);
	std::vector<std::string> names{ "Recommended", "Southpaw" };
	for (const auto& [name, _] : m_namedProfiles[player])
		names.push_back(name);
	std::ranges::sort(names);
	names.erase(std::unique(names.begin(), names.end()), names.end());
	return names;
}

std::string SampleInputMappingHost::ScopeKey(std::uint8_t player,
	std::optional<std::uint64_t> titleId)
{
	return titleId ? TitleProfileName(*titleId, player) :
		"global-p" + std::to_string(player + 1);
}

ControllerProfileDraft SampleInputMappingHost::LoadProfileDraft(
	std::uint8_t player, std::optional<std::uint64_t> titleId)
{
	player = std::min<std::uint8_t>(player, 7);
	const std::string key = ScopeKey(player, titleId);
	if (const auto found = m_savedProfiles.find(key); found != m_savedProfiles.end())
		return found->second;
	return MakeDefaultProfile(player, titleId, "xbox-one-series", {});
}

ControllerProfileDraft SampleInputMappingHost::LoadNamedProfileDraft(
	std::uint8_t player, std::string_view profileName,
	std::optional<std::uint64_t> titleId)
{
	player = std::min<std::uint8_t>(player, 7);
	ControllerProfileDraft draft;
	if (const auto found = m_namedProfiles[player].find(std::string(profileName));
		found != m_namedProfiles[player].end())
	{
		draft = found->second;
		draft.titleId = titleId;
		draft.player = player;
		draft.name = titleId ? TitleProfileName(*titleId, player) : found->first;
	}
	else
	{
		draft = MakeDefaultProfile(player, titleId, "xbox-one-series", {});
		if (!titleId)
			draft.name = std::string(profileName);
		if (profileName == "Southpaw")
		{
			auto find = [&draft](std::string_view id) {
				return std::ranges::find_if(draft.mappings,
					[id](const ControllerMappingDraft& value) {
						return value.emulatedControlId == id;
					});
			};
			for (const auto& [left, right] : std::array{
				std::pair{ "left_stick_up", "right_stick_up" },
				std::pair{ "left_stick_right", "right_stick_right" },
				std::pair{ "left_stick_down", "right_stick_down" },
				std::pair{ "left_stick_left", "right_stick_left" } })
			{
				auto leftMapping = find(left);
				auto rightMapping = find(right);
				if (leftMapping != draft.mappings.end() && rightMapping != draft.mappings.end())
					std::swap(leftMapping->expression, rightMapping->expression);
			}
		}
	}
	draft.sourceProfileName = std::string(profileName);
	draft.dirty = false;
	return draft;
}

ControllerProfileDraft SampleInputMappingHost::MakeDefaultProfile(
	std::uint8_t player, std::optional<std::uint64_t> titleId,
	std::string_view controllerTypeId, std::string_view preferredDeviceId)
{
	ControllerProfileDraft draft;
	draft.player = player;
	draft.titleId = titleId;
	draft.name = titleId ? TitleProfileName(*titleId, player) :
		"controller" + std::to_string(player);
	draft.sourceProfileName = "Recommended";
	draft.emulatedControllerTypeId = "xbox-one-series";
	const auto devices = EnumerateInputDevices();
	if (!preferredDeviceId.empty())
		draft.defaultDeviceId = std::string(preferredDeviceId);
	else if (const auto connected = std::ranges::find_if(devices,
		[](const InputDeviceDescriptor& value) { return value.connected; });
		connected != devices.end())
		draft.defaultDeviceId = connected->id;
	else if (!devices.empty())
		draft.defaultDeviceId = devices.front().id;
	if (!draft.defaultDeviceId.empty())
	{
		draft.attachedDeviceIds.push_back(draft.defaultDeviceId);
		draft.deviceSettings.push_back(CreateInputDeviceSettings(draft.defaultDeviceId));
	}
	draft.controllerSettings = CreateControllerSettings(player,
		draft.emulatedControllerTypeId);

	const std::unordered_map<std::string, std::string> defaults{
		{ "dpad_up", "DPAD-Up" }, { "dpad_right", "DPAD-Right" },
		{ "dpad_down", "DPAD-Down" }, { "dpad_left", "DPAD-Left" },
		{ "left_stick_click", "LeftThumb" }, { "left_stick_up", "LeftStickY+" },
		{ "left_stick_right", "LeftStickX+" }, { "left_stick_down", "LeftStickY-" },
		{ "left_stick_left", "LeftStickX-" }, { "right_stick_click", "RightThumb" },
		{ "right_stick_up", "RightStickY+" }, { "right_stick_right", "RightStickX+" },
		{ "right_stick_down", "RightStickY-" }, { "right_stick_left", "RightStickX-" },
		{ "a", "A" }, { "b", "B" }, { "x", "X" }, { "y", "Y" },
		{ "l", "LeftShoulder" },
		{ "r", "RightShoulder" }, { "zl", "LeftTrigger" },
		{ "zr", "RightTrigger" }, { "plus", "Menu" }, { "minus", "View" },
	};
	for (const EmulatedControlDescriptor& control : ControlsFor(
		draft.emulatedControllerTypeId))
	{
		ControllerMappingDraft mapping{ .emulatedControlId = control.id };
		if (!draft.defaultDeviceId.empty())
		{
			if (const auto found = defaults.find(control.id); found != defaults.end())
				mapping.expression = InputReference(draft.defaultDeviceId, found->second);
		}
		draft.mappings.push_back(std::move(mapping));
	}
	const auto servers = EnumerateDsuServers();
	if (const auto enabled = std::ranges::find_if(servers,
		[](const DsuServerModel& server) { return server.enabled; });
		enabled != servers.end())
	{
		draft.dsuRoute = DsuPlayerRoute{ .player = player,
			.serverId = enabled->id, .remoteSlot = player,
			.mode = DsuRouteMode::Merge, .gamepad = true, .motion = true,
			.touch = true };
	}
	draft.dirty = false;
	return draft;
}

ControllerProfileDraft SampleInputMappingHost::CreateDefaultProfile(
	std::uint8_t player, std::optional<std::uint64_t> titleId,
	std::string_view controllerTypeId, std::string_view preferredDeviceId)
{
	player = std::min<std::uint8_t>(player, 7);
	return MakeDefaultProfile(player, titleId, controllerTypeId,
		preferredDeviceId);
}

MappingSaveResult SampleInputMappingHost::SaveProfileDraft(
	const ControllerProfileDraft& draft)
{
	return SaveProfileDrafts(std::span<const ControllerProfileDraft>(&draft, 1));
}

MappingSaveResult SampleInputMappingHost::SaveProfileDrafts(
	std::span<const ControllerProfileDraft> drafts)
{
	std::set<std::uint8_t> players;
	for (const ControllerProfileDraft& draft : drafts)
	{
		if (draft.player >= 8 || draft.name.empty() ||
			draft.emulatedControllerTypeId != "xbox-one-series" ||
			!players.insert(draft.player).second)
		{
			return { false, "The sample profile batch is invalid" };
		}
	}
	if (const MappingSaveResult servers = ValidateDsuServers(drafts);
		!servers.succeeded)
	{
		return servers;
	}

	const auto previousSaved = m_savedProfiles;
	const auto previousNamed = m_namedProfiles;
	const DsuRouteTable previousRoutes = m_appliedDsuRoutes;
	for (const ControllerProfileDraft& draft : drafts)
	{
		ControllerProfileDraft saved = draft;
		saved.dirty = false;
		m_savedProfiles[ScopeKey(saved.player, saved.titleId)] = saved;
		if (!saved.titleId &&
			saved.name != "controller" + std::to_string(saved.player))
		{
			m_namedProfiles[saved.player][saved.name] = saved;
		}
	}
	DsuRouteTable routes;
	if (const MappingSaveResult applied = BuildAppliedRoutes(m_activeTitleId, routes);
		!applied.succeeded)
	{
		m_savedProfiles = previousSaved;
		m_namedProfiles = previousNamed;
		m_appliedDsuRoutes = previousRoutes;
		return applied;
	}
	m_appliedDsuRoutes = std::move(routes);
	if (const MappingSaveResult persisted = PersistProfiles(); !persisted.succeeded)
	{
		m_savedProfiles = previousSaved;
		m_namedProfiles = previousNamed;
		m_appliedDsuRoutes = previousRoutes;
		return persisted;
	}
	return { true, {} };
}

MappingSaveResult SampleInputMappingHost::DeleteNamedProfile(
	std::string_view profileName)
{
	if (profileName == "Recommended" || profileName == "Southpaw")
		return { false, "Built-in sample profiles cannot be deleted" };
	const auto previous = m_namedProfiles;
	bool removed = false;
	for (auto& profiles : m_namedProfiles)
		removed |= profiles.erase(std::string(profileName)) != 0;
	if (!removed)
		return { false, "The profile was not found" };
	if (const MappingSaveResult persisted = PersistProfiles(); !persisted.succeeded)
	{
		m_namedProfiles = previous;
		return persisted;
	}
	return { true, {} };
}

ControllerProfileStoreSnapshot SampleInputMappingHost::StoreSnapshot() const
{
	ControllerProfileStoreSnapshot snapshot;
	snapshot.activeProfiles.reserve(m_savedProfiles.size());
	for (const auto& [_, draft] : m_savedProfiles)
		snapshot.activeProfiles.push_back(draft);
	for (const auto& profiles : m_namedProfiles)
	{
		for (const auto& [_, draft] : profiles)
			snapshot.namedProfiles.push_back(draft);
	}
	auto order = [](const ControllerProfileDraft& left,
		const ControllerProfileDraft& right) {
		if (left.player != right.player)
			return left.player < right.player;
		if (left.titleId != right.titleId)
			return left.titleId < right.titleId;
		return left.name < right.name;
	};
	std::ranges::sort(snapshot.activeProfiles, order);
	std::ranges::sort(snapshot.namedProfiles, order);
	return snapshot;
}

MappingSaveResult SampleInputMappingHost::PersistProfiles() const
{
	if (m_profileStorePath.empty())
		return { true, {} };
	std::string error;
	if (!SaveControllerProfileStoreAtomic(
		m_profileStorePath, StoreSnapshot(), &error))
	{
		return { false, std::move(error) };
	}
	return { true, {} };
}

MappingSaveResult SampleInputMappingHost::ValidateDsuServers(
	std::span<const ControllerProfileDraft> drafts) const
{
	const auto servers = m_enumerateDsuServers ? m_enumerateDsuServers() :
		std::vector<DsuServerModel>{};
	for (const ControllerProfileDraft& draft : drafts)
	{
		if (!draft.dsuRoute)
			continue;
		const auto server = std::ranges::find_if(servers,
			[&draft](const DsuServerModel& value) {
				return value.id == draft.dsuRoute->serverId;
			});
		if (server == servers.end())
			return { false, "The selected DSU server no longer exists" };
		if (!server->enabled)
			return { false, "Enable the selected DSU server before applying its route" };
	}
	return { true, {} };
}

MappingSaveResult SampleInputMappingHost::BuildAppliedRoutes(
	std::optional<std::uint64_t> titleId, DsuRouteTable& routes) const
{
	std::vector<ControllerProfileDraft> profiles;
	profiles.reserve(8);
	for (std::uint8_t player = 0; player < 8; ++player)
	{
		auto profile = m_savedProfiles.end();
		if (titleId)
			profile = m_savedProfiles.find(ScopeKey(player, titleId));
		if (profile == m_savedProfiles.end())
			profile = m_savedProfiles.find(ScopeKey(player, std::nullopt));
		if (profile != m_savedProfiles.end())
			profiles.push_back(profile->second);
	}
	return routes.Apply(profiles);
}

MappingSaveResult SampleInputMappingHost::ConnectInputDevice(std::string_view)
{
	return { true, {} };
}

MappingSaveResult SampleInputMappingHost::CalibrateInputDevice(std::string_view)
{
	return { true, {} };
}

std::vector<InputCaptureSample> SampleInputMappingHost::ReadLiveInputs()
{
	return m_readInputs ? m_readInputs() : std::vector<InputCaptureSample>{};
}

void SampleInputMappingHost::TestRumble(std::string_view, float,
	std::chrono::milliseconds)
{
}

void SampleInputMappingHost::ResetMappingRuntime()
{
	DsuRouteTable routes;
	if (const MappingSaveResult applied = BuildAppliedRoutes(m_activeTitleId, routes);
		applied.succeeded)
	{
		m_appliedDsuRoutes = std::move(routes);
	}
}
}
