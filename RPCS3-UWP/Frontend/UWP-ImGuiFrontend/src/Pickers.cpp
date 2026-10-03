#include "UwpImGuiFrontend/Pickers.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>

#include <fmt/format.h>

#include <cstdint>
#include <utility>

namespace UwpImGuiFrontend
{
namespace
{
namespace Storage = winrt::Windows::Storage;
namespace AccessCache = winrt::Windows::Storage::AccessCache;
namespace Pickers = winrt::Windows::Storage::Pickers;

Pickers::PickerLocationId ToPlatformLocation(PickerStartLocation location)
{
	switch (location)
	{
	case PickerStartLocation::Documents:
		return Pickers::PickerLocationId::DocumentsLibrary;
	case PickerStartLocation::Pictures:
		return Pickers::PickerLocationId::PicturesLibrary;
	case PickerStartLocation::Videos:
		return Pickers::PickerLocationId::VideosLibrary;
	case PickerStartLocation::Music:
		return Pickers::PickerLocationId::MusicLibrary;
	case PickerStartLocation::Desktop:
		return Pickers::PickerLocationId::Desktop;
	case PickerStartLocation::Downloads:
		return Pickers::PickerLocationId::Downloads;
	case PickerStartLocation::Computer:
	default:
		return Pickers::PickerLocationId::ComputerFolder;
	}
}

struct PickerState
{
	std::atomic_bool open = false;
	std::atomic<std::uint64_t> generation = 0;
};

void ReportOpenState(const std::shared_ptr<PickerState>& state,
	std::uint64_t generation, const PickerCallbacks& callbacks, bool open)
{
	if (state->generation.load() != generation)
		return;
	state->open = open;
	if (callbacks.openStateChanged)
		callbacks.openStateChanged(open);
}

void ReportFailure(const std::shared_ptr<PickerState>& state,
	std::uint64_t generation, const PickerCallbacks& callbacks,
	std::string message)
{
	if (state->generation.load() != generation)
		return;
	ReportOpenState(state, generation, callbacks, false);
	if (callbacks.failed)
		callbacks.failed(message);
}

void ReportCompletion(const std::shared_ptr<PickerState>& state,
	std::uint64_t generation, const PickerCallbacks& callbacks,
	std::unique_ptr<PickedItem> result)
{
	if (state->generation.load() != generation)
		return;
	ReportOpenState(state, generation, callbacks, false);
	if (callbacks.completed)
		callbacks.completed(std::move(result));
}

template<typename TStorageItem>
void RememberSelection(const PickerRequest& request, const TStorageItem& item,
	PickedItem& result)
{
	if (request.futureAccessToken.empty())
		return;
	AccessCache::StorageApplicationPermissions::FutureAccessList().AddOrReplace(
		winrt::hstring(request.futureAccessToken), item);
	result.futureAccessToken = request.futureAccessToken;
}

winrt::fire_and_forget RunFilePicker(std::shared_ptr<PickerState> state,
	std::uint64_t generation, PickerRequest request, PickerCallbacks callbacks,
	winrt::Windows::UI::Core::CoreDispatcher dispatcher)
{
	winrt::apartment_context caller;
	std::unique_ptr<PickedItem> result;
	std::string failure;
	try
	{
		co_await winrt::resume_foreground(dispatcher);
		if (state->generation.load() != generation) co_return;
		Pickers::FileOpenPicker picker;
		picker.SuggestedStartLocation(ToPlatformLocation(request.startLocation));
		if (request.fileTypes.empty())
			picker.FileTypeFilter().Append(L"*");
		else
		{
			for (const std::wstring& type : request.fileTypes)
				picker.FileTypeFilter().Append(winrt::hstring(type));
		}
		Storage::StorageFile file = co_await picker.PickSingleFileAsync();
		if (!file)
		{
			co_await caller;
			ReportCompletion(state, generation, callbacks, {});
			co_return;
		}
		result = std::make_unique<PickedItem>();
		result->path = std::filesystem::path(std::wstring(file.Path().c_str()));
		result->name = file.Name().c_str();
		RememberSelection(request, file, *result);
	}
	catch (const winrt::hresult_error& error)
	{
		failure = fmt::format("{} (0x{:08X})", winrt::to_string(error.message()),
			static_cast<std::uint32_t>(error.code().value));
	}
	catch (const std::exception& error)
	{
		failure = error.what();
	}
	co_await caller;
	if (!failure.empty()) ReportFailure(state, generation, callbacks, std::move(failure));
	else ReportCompletion(state, generation, callbacks, std::move(result));
}

winrt::fire_and_forget RunFolderPicker(std::shared_ptr<PickerState> state,
	std::uint64_t generation, PickerRequest request, PickerCallbacks callbacks,
	winrt::Windows::UI::Core::CoreDispatcher dispatcher)
{
	winrt::apartment_context caller;
	std::unique_ptr<PickedItem> result;
	std::string failure;
	try
	{
		co_await winrt::resume_foreground(dispatcher);
		if (state->generation.load() != generation) co_return;
		Pickers::FolderPicker picker;
		picker.SuggestedStartLocation(ToPlatformLocation(request.startLocation));
		if (request.fileTypes.empty())
			picker.FileTypeFilter().Append(L"*");
		else
		{
			for (const std::wstring& type : request.fileTypes)
				picker.FileTypeFilter().Append(winrt::hstring(type));
		}
		Storage::StorageFolder folder = co_await picker.PickSingleFolderAsync();
		if (!folder)
		{
			co_await caller;
			ReportCompletion(state, generation, callbacks, {});
			co_return;
		}
		result = std::make_unique<PickedItem>();
		result->path = std::filesystem::path(std::wstring(folder.Path().c_str()));
		result->name = folder.Name().c_str();
		result->folder = true;
		RememberSelection(request, folder, *result);
	}
	catch (const winrt::hresult_error& error)
	{
		failure = fmt::format("{} (0x{:08X})", winrt::to_string(error.message()),
			static_cast<std::uint32_t>(error.code().value));
	}
	catch (const std::exception& error)
	{
		failure = error.what();
	}
	co_await caller;
	if (!failure.empty()) ReportFailure(state, generation, callbacks, std::move(failure));
	else ReportCompletion(state, generation, callbacks, std::move(result));
}

class PickerService final : public IPickerService
{
public:
	explicit PickerService(winrt::Windows::UI::Core::CoreDispatcher dispatcher)
		: m_state(std::make_shared<PickerState>()), m_dispatcher(std::move(dispatcher)) {}
	~PickerService() override { CancelCallbacks(); }

	bool PickFile(PickerRequest request, PickerCallbacks callbacks) override
	{
		std::uint64_t generation = 0;
		if (!Begin(callbacks, generation))
			return false;
		RunFilePicker(m_state, generation, std::move(request), std::move(callbacks), m_dispatcher);
		return true;
	}

	bool PickFolder(PickerRequest request, PickerCallbacks callbacks) override
	{
		std::uint64_t generation = 0;
		if (!Begin(callbacks, generation))
			return false;
		RunFolderPicker(m_state, generation, std::move(request), std::move(callbacks), m_dispatcher);
		return true;
	}

	bool IsOpen() const noexcept override
	{
		return m_state->open.load();
	}

	void CancelCallbacks() noexcept override
	{
		m_state->generation.fetch_add(1);
		m_state->open = false;
	}

private:
	bool Begin(const PickerCallbacks& callbacks, std::uint64_t& generation)
	{
		bool expected = false;
		if (!m_state->open.compare_exchange_strong(expected, true))
			return false;
		generation = m_state->generation.fetch_add(1) + 1;
		if (callbacks.openStateChanged)
			callbacks.openStateChanged(true);
		return true;
	}

	std::shared_ptr<PickerState> m_state;
	winrt::Windows::UI::Core::CoreDispatcher m_dispatcher;
};
}

std::unique_ptr<IPickerService> CreatePickerService(winrt::Windows::UI::Core::CoreDispatcher dispatcher)
{
	return std::make_unique<PickerService>(std::move(dispatcher));
}
}
