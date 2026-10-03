#pragma once

#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace UwpImGuiFrontend
{
enum class ScreenScraperState : std::uint8_t
{
	Idle,
	Working,
	Finished,
	UpToDate,
	Error,
	QuotaPaused,
};

enum class ScreenScraperRegion : std::uint8_t
{
	Automatic,
	UnitedStates,
	Europe,
	Japan,
	World,
};

enum class ScreenScraperArtwork : std::uint8_t
{
	Box2D,
	Box3D,
	MixRecalboxV1,
	MixRecalboxV2,
	FanArt,
	Screenshot,
	Logo,
	BackCover,
	PhysicalMedia,
};

enum class ScreenScraperBackground : std::uint8_t
{
	Off,
	FanArt,
	Screenshot,
	Video,
	MixRecalboxV1,
	MixRecalboxV2,
};

[[nodiscard]] std::string_view ScreenScraperArtworkLabel(
	ScreenScraperArtwork artwork) noexcept;
[[nodiscard]] std::string_view ScreenScraperArtworkFileName(
	ScreenScraperArtwork artwork) noexcept;
[[nodiscard]] MediaKind ScreenScraperArtworkMediaKind(
	ScreenScraperArtwork artwork) noexcept;

// ES-DE media path: <root>/<system>/<type>/<ROM-relative stem>.<ext>.
struct EsDeMediaLayout
{
	std::filesystem::path rootDirectory;
	std::string systemName;
	std::filesystem::path relativeGamePath;
};

[[nodiscard]] EsDeMediaLayout MakeEsDeMediaLayout(
	std::filesystem::path rootDirectory, std::string systemName,
	const std::filesystem::path& contentPath,
	const std::filesystem::path& contentRoot = {});
[[nodiscard]] std::filesystem::path ResolveEsDeArtworkPath(
	const EsDeMediaLayout& layout, ScreenScraperArtwork artwork,
	std::string_view extension = ".png");
[[nodiscard]] std::filesystem::path ResolveEsDeVideoPath(
	const EsDeMediaLayout& layout, std::string_view extension = ".mp4");
[[nodiscard]] std::optional<std::filesystem::path> FindExistingEsDeArtworkPath(
	const EsDeMediaLayout& layout, ScreenScraperArtwork artwork);
[[nodiscard]] std::optional<std::filesystem::path> FindExistingEsDeVideoPath(
	const EsDeMediaLayout& layout);

// Converts ScreenScraper's 0-20 "note" value to normalized 0-1 text.
[[nodiscard]] std::string NormalizeScreenScraperApiRating(
	std::string_view value);

// Decodes HTML/XML character references in API text.
[[nodiscard]] std::string NormalizeScreenScraperText(std::string_view value);

// Resolves Automatic from the supplied game-region hint.
[[nodiscard]] std::string ResolveScreenScraperRegionCode(
	ScreenScraperRegion preferredRegion, std::string_view automaticRegionHint = {});

struct ScreenScraperCredentials
{
	std::string user;
	std::string password;
};

struct ScreenScraperDeveloperIdentity
{
	std::string id;
	std::string password;
	std::string softwareName;
};

struct ScreenScraperOptions
{
	bool downloadMetadata = true;
	bool downloadBoxArt = true;
	bool downloadFanArt = true;
	bool downloadVideos = false;
	bool downloadScreenshots = true;
	bool downloadLogos = true;
	bool overwriteExisting = false;
	// When unset, platform media follows overwriteExisting.
	std::optional<bool> overwritePlatformMedia;
	ScreenScraperRegion preferredRegion = ScreenScraperRegion::Automatic;
};

struct ScreenScraperTarget
{
	ItemId id = 0;
	std::uint32_t systemId = 0;
	std::uint64_t matchedGameId = 0;
	std::string name;
	std::string region;
	std::filesystem::path contentPath;
	// Per-item metadata and manifest directory.
	std::filesystem::path mediaDirectory;
	// Optional ES-DE media destination.
	std::optional<EsDeMediaLayout> esDeMedia;
};

struct ScreenScraperRequest
{
	ScreenScraperDeveloperIdentity developer;
	ScreenScraperCredentials account;
	ScreenScraperOptions options;
	std::filesystem::path platformMediaDirectory;
	std::filesystem::path progressFile;
	std::uint32_t platformSystemId = 0;
	std::vector<ScreenScraperTarget> targets;
};

struct ScreenScraperProgress
{
	ScreenScraperState state = ScreenScraperState::Idle;
	std::size_t completedTargets = 0;
	std::size_t totalTargets = 0;
	std::size_t downloadedFiles = 0;
	std::string status;
};

struct ScreenScraperMediaReference
{
	std::string type;
	std::string parent;
	std::string region;
	std::string format;
};

struct ScreenScraperArtworkPreview
{
	ScreenScraperArtwork artwork = ScreenScraperArtwork::Box2D;
	std::uint32_t maximumHeight = 0;
	std::filesystem::path path;
};

struct ScreenScraperMatch
{
	std::uint64_t gameId = 0;
	std::uint32_t systemId = 0;
	std::string title;
	std::string platform;
	std::string region;
	Metadata metadata;
	std::vector<ScreenScraperMediaReference> media;
	std::vector<ScreenScraperArtworkPreview> previews;
};

enum class ScreenScraperSearchState : std::uint8_t
{
	Idle,
	Searching,
	Ready,
	NoMatches,
	Error,
};

struct ScreenScraperSearchRequest
{
	ScreenScraperDeveloperIdentity developer;
	ScreenScraperCredentials account;
	ScreenScraperRegion preferredRegion = ScreenScraperRegion::Automatic;
	std::uint32_t systemId = 0;
	// Fallback when a match omits its platform label.
	std::string platformName;
	// Resolves Automatic for preview media.
	std::string regionHint;
	std::string query;
	std::filesystem::path previewDirectory;
};

struct ScreenScraperSearchSnapshot
{
	std::uint64_t generation = 0;
	ScreenScraperSearchState state = ScreenScraperSearchState::Idle;
	std::string query;
	std::string status;
	std::vector<ScreenScraperMatch> matches;
};

struct GameMetadataRecord
{
	ItemId itemId = 0;
	std::uint64_t sourceGameId = 0;
	std::string title;
	Metadata metadata;
};

struct ScreenScraperMediaManifest
{
	std::uint64_t gameId = 0;
	std::vector<std::string> available;
	std::vector<std::string> saved;
};

class IScreenScraperService
{
public:
	virtual ~IScreenScraperService() = default;

	[[nodiscard]] virtual ScreenScraperProgress GetProgress() const = 0;
	virtual bool Start(ScreenScraperRequest request) = 0;
	[[nodiscard]] virtual bool ConsumeFinished() = 0;
	virtual void RequestStop() = 0;
	[[nodiscard]] virtual bool HasPendingScan(const std::filesystem::path& progressFile) const = 0;
	virtual bool ResumePendingScan(ScreenScraperRequest defaults) = 0;
	[[nodiscard]] virtual std::optional<std::vector<std::string>> GetAvailableMediaTypes(
		const std::filesystem::path& mediaDirectory) const = 0;

	// Starting a search invalidates previous results and previews.
	virtual std::uint64_t BeginSearch(ScreenScraperSearchRequest request) = 0;
	[[nodiscard]] virtual ScreenScraperSearchSnapshot GetSearchSnapshot() const = 0;
	virtual void CancelSearch() = 0;
	virtual void RequestSearchPreview(std::uint64_t generation,
		std::uint64_t gameId, ScreenScraperArtwork artwork,
		std::uint32_t maximumHeight) = 0;
};

using ScreenScraperLog = std::function<void(LogLevel, std::string_view)>;

[[nodiscard]] Metadata LoadScreenScraperMetadata(
	const std::filesystem::path& metadataPath,
	std::string_view region = {});
void MergeMissingMetadata(Metadata& target, const Metadata& source);

[[nodiscard]] std::optional<GameMetadataRecord> LoadGameMetadataRecord(
	const std::filesystem::path& path);
[[nodiscard]] bool SaveGameMetadataRecordAtomic(
	const std::filesystem::path& path, const GameMetadataRecord& record);

[[nodiscard]] std::optional<ScreenScraperMediaManifest>
	LoadScreenScraperMediaManifest(const std::filesystem::path& path);

[[nodiscard]] const ScreenScraperArtworkPreview* FindPreview(
	const ScreenScraperMatch& match, ScreenScraperArtwork artwork) noexcept;

[[nodiscard]] std::unique_ptr<IScreenScraperService> CreateScreenScraperService(
	ScreenScraperLog log = {});
}
