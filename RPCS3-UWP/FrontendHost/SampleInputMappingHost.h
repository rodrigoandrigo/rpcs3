#pragma once

#include "UwpImGuiFrontend/ControllerProfileStore.h"

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

namespace UwpImGuiFrontend::Sample
{
class SampleInputMappingHost final : public IInputMappingHost
{
public:
	using DeviceEnumerator = std::function<std::vector<InputDeviceDescriptor>()>;
	using LiveInputReader = std::function<std::vector<InputCaptureSample>()>;
	using DsuServerEnumerator = std::function<std::vector<DsuServerModel>()>;
	using TextureLoader = std::function<TextureHandle(const std::filesystem::path&)>;
	using TextureReleaser = std::function<void(TextureHandle)>;

	~SampleInputMappingHost() override;

	void SetInputCallbacks(DeviceEnumerator enumerate, LiveInputReader read);
	void SetDsuServerEnumerator(DsuServerEnumerator enumerate);
	void SetControllerPreviewCallbacks(std::filesystem::path assetRoot,
		TextureLoader load, TextureReleaser release);
	[[nodiscard]] MappingSaveResult SetProfileStorePath(
		std::filesystem::path path);
	[[nodiscard]] const DsuPlayerRoute* AppliedDsuRoute(
		std::uint8_t player) const noexcept;
	[[nodiscard]] bool AcceptsDsuInput(std::uint8_t player,
		std::string_view serverId, std::uint8_t remoteSlot,
		DsuInputFeature feature) const;
	[[nodiscard]] MappingSaveResult ActivateProfileRoutes(
		std::optional<std::uint64_t> titleId);

	std::vector<InputDeviceDescriptor> EnumerateInputDevices() override;
	std::vector<DsuServerModel> EnumerateDsuServers() override;
	std::vector<EmulatedControllerDescriptor> EnumerateEmulatedControllerTypes(
		std::uint8_t player) override;
	TextureHandle GetControllerPreviewTexture(
		std::string_view controllerTypeId) override;
	std::vector<EmulatedControlDescriptor> EnumerateEmulatedControls(
		std::uint8_t player, std::string_view controllerTypeId) override;
	std::vector<InputSettingDraft> CreateControllerSettings(
		std::uint8_t player, std::string_view controllerTypeId) override;
	InputDeviceSettingsDraft CreateInputDeviceSettings(
		std::string_view deviceId) override;
	std::vector<std::string> EnumerateProfileNames(std::uint8_t player) override;
	ControllerProfileDraft LoadProfileDraft(std::uint8_t player,
		std::optional<std::uint64_t> titleId) override;
	ControllerProfileDraft LoadNamedProfileDraft(std::uint8_t player,
		std::string_view profileName,
		std::optional<std::uint64_t> titleId) override;
	ControllerProfileDraft CreateDefaultProfile(std::uint8_t player,
		std::optional<std::uint64_t> titleId,
		std::string_view controllerTypeId = {},
		std::string_view preferredDeviceId = {}) override;
	MappingSaveResult SaveProfileDraft(const ControllerProfileDraft& draft) override;
	MappingSaveResult SaveProfileDrafts(
		std::span<const ControllerProfileDraft> drafts) override;
	MappingSaveResult DeleteNamedProfile(std::string_view profileName) override;
	MappingSaveResult ConnectInputDevice(std::string_view deviceId) override;
	MappingSaveResult CalibrateInputDevice(std::string_view deviceId) override;
	std::vector<InputCaptureSample> ReadLiveInputs() override;
	void TestRumble(std::string_view deviceId, float strength,
		std::chrono::milliseconds duration) override;
	void ResetMappingRuntime() override;

private:
	[[nodiscard]] static std::string ScopeKey(std::uint8_t player,
		std::optional<std::uint64_t> titleId);
	[[nodiscard]] static std::vector<EmulatedControlDescriptor> ControlsFor(
		std::string_view controllerTypeId);
	[[nodiscard]] ControllerProfileDraft MakeDefaultProfile(std::uint8_t player,
		std::optional<std::uint64_t> titleId, std::string_view controllerTypeId,
		std::string_view preferredDeviceId);
	[[nodiscard]] ControllerProfileStoreSnapshot StoreSnapshot() const;
	[[nodiscard]] MappingSaveResult PersistProfiles() const;
	[[nodiscard]] MappingSaveResult ValidateDsuServers(
		std::span<const ControllerProfileDraft> drafts) const;
	[[nodiscard]] MappingSaveResult BuildAppliedRoutes(
		std::optional<std::uint64_t> titleId, DsuRouteTable& routes) const;

	DeviceEnumerator m_enumerateDevices;
	LiveInputReader m_readInputs;
	DsuServerEnumerator m_enumerateDsuServers;
	TextureLoader m_loadTexture;
	TextureReleaser m_releaseTexture;
	std::filesystem::path m_controllerPreviewRoot;
	std::unordered_map<std::string, TextureHandle> m_controllerPreviewTextures;
	std::filesystem::path m_profileStorePath;
	std::unordered_map<std::string, ControllerProfileDraft> m_savedProfiles;
	std::array<std::unordered_map<std::string, ControllerProfileDraft>, 8>
		m_namedProfiles;
	std::optional<std::uint64_t> m_activeTitleId;
	DsuRouteTable m_appliedDsuRoutes;
};
}
