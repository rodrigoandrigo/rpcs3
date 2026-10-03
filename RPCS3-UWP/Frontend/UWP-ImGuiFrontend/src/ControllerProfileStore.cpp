#include "UwpImGuiFrontend/ControllerProfileStore.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

namespace UwpImGuiFrontend
{
namespace
{
constexpr unsigned kStoreVersion = 1;
constexpr std::uintmax_t kMaximumStoreBytes = 16u * 1024u * 1024u;

using JsonWriter = rapidjson::Writer<rapidjson::StringBuffer>;

void SetError(std::string* error, std::string message)
{
	if (error)
		*error = std::move(message);
}

void WriteString(JsonWriter& writer, const char* name, std::string_view value)
{
	writer.Key(name);
	writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
}

void WriteSetting(JsonWriter& writer, const InputSettingDraft& setting)
{
	writer.StartObject();
	WriteString(writer, "id", setting.id);
	WriteString(writer, "label", setting.label);
	WriteString(writer, "group", setting.group);
	WriteString(writer, "description", setting.description);
	writer.Key("kind");
	writer.Uint(static_cast<unsigned>(setting.kind));
	writer.Key("enabled");
	writer.Bool(setting.enabled);
	writer.Key("boolValue");
	writer.Bool(setting.boolValue);
	writer.Key("numberValue");
	writer.Double(setting.numberValue);
	writer.Key("minimum");
	writer.Double(setting.minimum);
	writer.Key("maximum");
	writer.Double(setting.maximum);
	writer.Key("step");
	writer.Double(setting.step);
	WriteString(writer, "suffix", setting.suffix);
	WriteString(writer, "choiceValue", setting.choiceValue);
	writer.Key("choices");
	writer.StartArray();
	for (const InputSettingChoice& choice : setting.choices)
	{
		writer.StartObject();
		WriteString(writer, "value", choice.value);
		WriteString(writer, "label", choice.label);
		writer.EndObject();
	}
	writer.EndArray();
	writer.EndObject();
}

void WriteDraft(JsonWriter& writer, const ControllerProfileDraft& draft)
{
	writer.StartObject();
	WriteString(writer, "name", draft.name);
	WriteString(writer, "sourceProfileName", draft.sourceProfileName);
	writer.Key("player");
	writer.Uint(draft.player);
	writer.Key("titleId");
	if (draft.titleId)
		writer.Uint64(*draft.titleId);
	else
		writer.Null();
	WriteString(writer, "emulatedControllerTypeId", draft.emulatedControllerTypeId);

	writer.Key("controllerSettings");
	writer.StartArray();
	for (const InputSettingDraft& setting : draft.controllerSettings)
		WriteSetting(writer, setting);
	writer.EndArray();

	WriteString(writer, "defaultDeviceId", draft.defaultDeviceId);
	writer.Key("attachedDeviceIds");
	writer.StartArray();
	for (const std::string& deviceId : draft.attachedDeviceIds)
		writer.String(deviceId.data(), static_cast<rapidjson::SizeType>(deviceId.size()));
	writer.EndArray();

	writer.Key("deviceSettings");
	writer.StartArray();
	for (const InputDeviceSettingsDraft& device : draft.deviceSettings)
	{
		writer.StartObject();
		WriteString(writer, "deviceId", device.deviceId);
		writer.Key("settings");
		writer.StartArray();
		for (const InputSettingDraft& setting : device.settings)
			WriteSetting(writer, setting);
		writer.EndArray();
		writer.EndObject();
	}
	writer.EndArray();

	writer.Key("mappings");
	writer.StartArray();
	for (const ControllerMappingDraft& mapping : draft.mappings)
	{
		writer.StartObject();
		WriteString(writer, "control", mapping.emulatedControlId);
		WriteString(writer, "expression", mapping.expression);
		writer.EndObject();
	}
	writer.EndArray();

	writer.Key("dsuRoute");
	if (!draft.dsuRoute)
	{
		writer.Null();
	}
	else
	{
		const DsuPlayerRoute& route = *draft.dsuRoute;
		writer.StartObject();
		writer.Key("player");
		writer.Uint(route.player);
		WriteString(writer, "serverId", route.serverId);
		writer.Key("remoteSlot");
		writer.Uint(route.remoteSlot);
		writer.Key("mode");
		writer.Uint(static_cast<unsigned>(route.mode));
		writer.Key("gamepad");
		writer.Bool(route.gamepad);
		writer.Key("motion");
		writer.Bool(route.motion);
		writer.Key("touch");
		writer.Bool(route.touch);
		writer.EndObject();
	}
	writer.EndObject();
}

const rapidjson::Value* Member(const rapidjson::Value& object, const char* name)
{
	if (!object.IsObject())
		return nullptr;
	const auto found = object.FindMember(name);
	return found == object.MemberEnd() ? nullptr : &found->value;
}

bool ReadString(const rapidjson::Value& object, const char* name,
	std::string& value)
{
	const rapidjson::Value* member = Member(object, name);
	if (!member || !member->IsString())
		return false;
	value.assign(member->GetString(), member->GetStringLength());
	return true;
}

bool ReadBool(const rapidjson::Value& object, const char* name, bool& value)
{
	const rapidjson::Value* member = Member(object, name);
	if (!member || !member->IsBool())
		return false;
	value = member->GetBool();
	return true;
}

bool ReadFloat(const rapidjson::Value& object, const char* name, float& value)
{
	const rapidjson::Value* member = Member(object, name);
	if (!member || !member->IsNumber())
		return false;
	value = member->GetFloat();
	return std::isfinite(value);
}

bool ReadSetting(const rapidjson::Value& value, InputSettingDraft& setting)
{
	unsigned kind = 0;
	const rapidjson::Value* kindValue = Member(value, "kind");
	const rapidjson::Value* choices = Member(value, "choices");
	if (!ReadString(value, "id", setting.id) ||
		!ReadString(value, "label", setting.label) ||
		!ReadString(value, "group", setting.group) ||
		!ReadString(value, "description", setting.description) ||
		!kindValue || !kindValue->IsUint() ||
		(kind = kindValue->GetUint()) > static_cast<unsigned>(InputSettingKind::Choice) ||
		!ReadBool(value, "enabled", setting.enabled) ||
		!ReadBool(value, "boolValue", setting.boolValue) ||
		!ReadFloat(value, "numberValue", setting.numberValue) ||
		!ReadFloat(value, "minimum", setting.minimum) ||
		!ReadFloat(value, "maximum", setting.maximum) ||
		!ReadFloat(value, "step", setting.step) ||
		!ReadString(value, "suffix", setting.suffix) ||
		!ReadString(value, "choiceValue", setting.choiceValue) ||
		!choices || !choices->IsArray())
	{
		return false;
	}
	setting.kind = static_cast<InputSettingKind>(kind);
	setting.choices.clear();
	for (const rapidjson::Value& choiceValue : choices->GetArray())
	{
		InputSettingChoice choice;
		if (!ReadString(choiceValue, "value", choice.value) ||
			!ReadString(choiceValue, "label", choice.label))
		{
			return false;
		}
		setting.choices.push_back(std::move(choice));
	}
	return true;
}

bool ReadSettings(const rapidjson::Value& object, const char* name,
	std::vector<InputSettingDraft>& settings)
{
	const rapidjson::Value* values = Member(object, name);
	if (!values || !values->IsArray())
		return false;
	settings.clear();
	for (const rapidjson::Value& value : values->GetArray())
	{
		InputSettingDraft setting;
		if (!ReadSetting(value, setting))
			return false;
		settings.push_back(std::move(setting));
	}
	return true;
}

bool ReadDraft(const rapidjson::Value& value, ControllerProfileDraft& draft)
{
	const rapidjson::Value* player = Member(value, "player");
	const rapidjson::Value* titleId = Member(value, "titleId");
	const rapidjson::Value* attached = Member(value, "attachedDeviceIds");
	const rapidjson::Value* devices = Member(value, "deviceSettings");
	const rapidjson::Value* mappings = Member(value, "mappings");
	const rapidjson::Value* route = Member(value, "dsuRoute");
	if (!ReadString(value, "name", draft.name) ||
		!ReadString(value, "sourceProfileName", draft.sourceProfileName) ||
		!player || !player->IsUint() || player->GetUint() >= 8 ||
		!titleId || (!titleId->IsNull() && !titleId->IsUint64()) ||
		!ReadString(value, "emulatedControllerTypeId", draft.emulatedControllerTypeId) ||
		!ReadSettings(value, "controllerSettings", draft.controllerSettings) ||
		!ReadString(value, "defaultDeviceId", draft.defaultDeviceId) ||
		!attached || !attached->IsArray() || !devices || !devices->IsArray() ||
		!mappings || !mappings->IsArray() || !route)
	{
		return false;
	}
	draft.player = static_cast<std::uint8_t>(player->GetUint());
	draft.titleId = titleId->IsUint64() ?
		std::optional<std::uint64_t>(titleId->GetUint64()) : std::nullopt;
	draft.attachedDeviceIds.clear();
	for (const rapidjson::Value& deviceId : attached->GetArray())
	{
		if (!deviceId.IsString())
			return false;
		draft.attachedDeviceIds.emplace_back(
			deviceId.GetString(), deviceId.GetStringLength());
	}
	draft.deviceSettings.clear();
	for (const rapidjson::Value& deviceValue : devices->GetArray())
	{
		InputDeviceSettingsDraft device;
		if (!ReadString(deviceValue, "deviceId", device.deviceId) ||
			!ReadSettings(deviceValue, "settings", device.settings))
		{
			return false;
		}
		draft.deviceSettings.push_back(std::move(device));
	}
	draft.mappings.clear();
	for (const rapidjson::Value& mappingValue : mappings->GetArray())
	{
		ControllerMappingDraft mapping;
		if (!ReadString(mappingValue, "control", mapping.emulatedControlId) ||
			!ReadString(mappingValue, "expression", mapping.expression))
		{
			return false;
		}
		draft.mappings.push_back(std::move(mapping));
	}
	if (route->IsNull())
	{
		draft.dsuRoute.reset();
	}
	else
	{
		const rapidjson::Value* routePlayer = Member(*route, "player");
		const rapidjson::Value* remoteSlot = Member(*route, "remoteSlot");
		const rapidjson::Value* mode = Member(*route, "mode");
		DsuPlayerRoute parsed;
		if (!routePlayer || !routePlayer->IsUint() || routePlayer->GetUint() >= 8 ||
			!ReadString(*route, "serverId", parsed.serverId) || parsed.serverId.empty() ||
			!remoteSlot || !remoteSlot->IsUint() || remoteSlot->GetUint() >= 16 ||
			!mode || !mode->IsUint() ||
			mode->GetUint() > static_cast<unsigned>(DsuRouteMode::Merge) ||
			!ReadBool(*route, "gamepad", parsed.gamepad) ||
			!ReadBool(*route, "motion", parsed.motion) ||
			!ReadBool(*route, "touch", parsed.touch))
		{
			return false;
		}
		parsed.player = static_cast<std::uint8_t>(routePlayer->GetUint());
		parsed.remoteSlot = static_cast<std::uint8_t>(remoteSlot->GetUint());
		parsed.mode = static_cast<DsuRouteMode>(mode->GetUint());
		draft.dsuRoute = std::move(parsed);
	}
	draft.dirty = false;
	return !draft.name.empty() && !draft.emulatedControllerTypeId.empty();
}

bool ReadProfileArray(const rapidjson::Value& document, const char* name,
	std::vector<ControllerProfileDraft>& profiles)
{
	const rapidjson::Value* values = Member(document, name);
	if (!values || !values->IsArray() || values->Size() > 4096)
		return false;
	profiles.clear();
	for (const rapidjson::Value& value : values->GetArray())
	{
		ControllerProfileDraft draft;
		if (!ReadDraft(value, draft))
			return false;
		profiles.push_back(std::move(draft));
	}
	return true;
}

bool RouteFeatureEnabled(const DsuPlayerRoute& route, DsuInputFeature feature)
{
	switch (feature)
	{
	case DsuInputFeature::Gamepad: return route.gamepad;
	case DsuInputFeature::Motion: return route.motion;
	case DsuInputFeature::Touch: return route.touch;
	}
	return false;
}
}

bool LoadControllerProfileStore(const std::filesystem::path& path,
	ControllerProfileStoreSnapshot& snapshot, std::string* error)
{
	if (error)
		error->clear();
	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
	{
		if (ec)
		{
			SetError(error, "Unable to inspect the controller profile store: " + ec.message());
			return false;
		}
		snapshot = {};
		return true;
	}
	const std::uintmax_t size = std::filesystem::file_size(path, ec);
	if (ec || size > kMaximumStoreBytes)
	{
		SetError(error, ec ? "Unable to inspect the controller profile store: " +
			ec.message() : "The controller profile store is too large");
		return false;
	}
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		SetError(error, "Unable to open the controller profile store");
		return false;
	}
	std::string data(static_cast<std::size_t>(size), '\0');
	input.read(data.data(), static_cast<std::streamsize>(data.size()));
	if (!input && !data.empty())
	{
		SetError(error, "Unable to read the controller profile store");
		return false;
	}
	rapidjson::Document document;
	document.Parse(data.data(), data.size());
	if (document.HasParseError())
	{
		SetError(error, std::string("Invalid controller profile store: ") +
			rapidjson::GetParseError_En(document.GetParseError()));
		return false;
	}
	const rapidjson::Value* version = Member(document, "version");
	ControllerProfileStoreSnapshot loaded;
	if (!version || !version->IsUint() || version->GetUint() != kStoreVersion ||
		!ReadProfileArray(document, "activeProfiles", loaded.activeProfiles) ||
		!ReadProfileArray(document, "namedProfiles", loaded.namedProfiles))
	{
		SetError(error, "The controller profile store has an unsupported structure");
		return false;
	}
	for (const ControllerProfileDraft& named : loaded.namedProfiles)
	{
		if (named.titleId)
		{
			SetError(error, "A named controller profile contains a title assignment");
			return false;
		}
	}
	snapshot = std::move(loaded);
	return true;
}

bool SaveControllerProfileStoreAtomic(const std::filesystem::path& path,
	const ControllerProfileStoreSnapshot& snapshot, std::string* error)
{
	if (error)
		error->clear();
	rapidjson::StringBuffer buffer;
	JsonWriter writer(buffer);
	writer.StartObject();
	writer.Key("version");
	writer.Uint(kStoreVersion);
	writer.Key("activeProfiles");
	writer.StartArray();
	for (const ControllerProfileDraft& draft : snapshot.activeProfiles)
		WriteDraft(writer, draft);
	writer.EndArray();
	writer.Key("namedProfiles");
	writer.StartArray();
	for (const ControllerProfileDraft& draft : snapshot.namedProfiles)
		WriteDraft(writer, draft);
	writer.EndArray();
	writer.EndObject();

	std::error_code ec;
	if (!path.parent_path().empty())
		std::filesystem::create_directories(path.parent_path(), ec);
	if (ec)
	{
		SetError(error, "Unable to create the controller profile directory: " + ec.message());
		return false;
	}
	std::filesystem::path temporary = path;
	temporary += ".tmp";
	std::filesystem::path backup = path;
	backup += ".bak";
	std::filesystem::remove(temporary, ec);
	ec.clear();
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			SetError(error, "Unable to create the temporary controller profile store");
			return false;
		}
		output.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
		output.flush();
		if (!output)
		{
			output.close();
			std::filesystem::remove(temporary, ec);
			SetError(error, "Unable to write the controller profile store");
			return false;
		}
	}

	const bool hadOriginal = std::filesystem::exists(path, ec) && !ec;
	if (ec)
	{
		std::filesystem::remove(temporary, ec);
		SetError(error, "Unable to inspect the previous controller profile store");
		return false;
	}
	if (hadOriginal)
	{
		std::filesystem::remove(backup, ec);
		ec.clear();
		std::filesystem::rename(path, backup, ec);
		if (ec)
		{
			const std::string message = ec.message();
			std::filesystem::remove(temporary, ec);
			SetError(error, "Unable to prepare the previous controller profile store: " +
				message);
			return false;
		}
	}
	std::filesystem::rename(temporary, path, ec);
	if (ec)
	{
		const std::string message = ec.message();
		if (hadOriginal)
		{
			std::error_code restoreError;
			std::filesystem::rename(backup, path, restoreError);
		}
		std::filesystem::remove(temporary, ec);
		SetError(error, "Unable to replace the controller profile store: " + message);
		return false;
	}
	if (hadOriginal)
		std::filesystem::remove(backup, ec);
	return true;
}

MappingSaveResult DsuRouteTable::Apply(
	std::span<const ControllerProfileDraft> profiles)
{
	auto routes = m_routes;
	for (const ControllerProfileDraft& profile : profiles)
	{
		if (profile.player >= routes.size())
			return { false, "The DSU route uses an invalid player" };
		routes[profile.player] = profile.dsuRoute;
		if (!routes[profile.player])
			continue;
		DsuPlayerRoute& route = *routes[profile.player];
		if (route.player != profile.player)
			return { false, "The DSU route player does not match its profile" };
		if (route.serverId.empty())
			return { false, "The DSU route has no server" };
		if (route.remoteSlot >= 16)
			return { false, "The DSU remote slot must be between 0 and 15" };
		if (!route.gamepad && !route.motion && !route.touch)
			return { false, "The DSU route must enable gamepad, motion, or touch input" };
		if (route.mode == DsuRouteMode::FullController && !route.gamepad)
			return { false, "Full Controller mode requires gamepad input" };
	}
	std::set<std::pair<std::string, std::uint8_t>> claims;
	for (const auto& route : routes)
	{
		if (route && !claims.emplace(route->serverId, route->remoteSlot).second)
			return { false, "Two players cannot use the same DSU server and remote slot" };
	}
	m_routes = std::move(routes);
	return { true, {} };
}

void DsuRouteTable::Clear() noexcept
{
	for (auto& route : m_routes)
		route.reset();
}

const DsuPlayerRoute* DsuRouteTable::RouteForPlayer(
	std::uint8_t player) const noexcept
{
	return player < m_routes.size() && m_routes[player] ? &*m_routes[player] : nullptr;
}

bool DsuRouteTable::Accepts(std::uint8_t player, std::string_view serverId,
	std::uint8_t remoteSlot, DsuInputFeature feature) const noexcept
{
	const DsuPlayerRoute* route = RouteForPlayer(player);
	return route && route->serverId == serverId && route->remoteSlot == remoteSlot &&
		RouteFeatureEnabled(*route, feature);
}

bool DsuRouteTable::UsesAttachedDevices(std::uint8_t player) const noexcept
{
	const DsuPlayerRoute* route = RouteForPlayer(player);
	return !route || route->mode == DsuRouteMode::Merge;
}
}
