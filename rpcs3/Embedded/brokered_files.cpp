#include "stdafx.h"
#include "brokered_files.h"
#include "brokered_path.h"
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <Windows.h>
#include <mutex>
#include <limits>
#include <unordered_set>

LOG_CHANNEL(broker_log, "BrokeredFS");

namespace rpcs3::embedded
{
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

namespace
{
struct apartment
{
    apartment() { winrt::check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~apartment() { CoUninitialize(); }
};

void io_error(const winrt::hresult_error& error, const std::string& path = {})
{
    const HRESULT code = error.code();
    fs::g_tls_error = code == E_INVALIDARG ? fs::error::inval :
        code == HRESULT_FROM_WIN32(ERROR_DIRECTORY) ? fs::error::notdir :
        code == E_ACCESSDENIED ? fs::error::acces :
        code == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || code == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
        ? fs::error::noent : fs::error::unknown;
    if (fs::g_tls_error == fs::error::noent || fs::g_tls_error == fs::error::notdir)
        broker_log.trace("Storage item not found: HRESULT=0x%08x: %s",
            static_cast<u32>(code), winrt::to_string(error.message()));
    else
        broker_log.error("Storage operation failed: path='%s', HRESULT=0x%08x: %s",
            path, static_cast<u32>(code), winrt::to_string(error.message()));
}

bool allowed_mode(bs_t<fs::open_mode> mode, bool writable)
{
    if (!writable && (mode & (fs::write + fs::create + fs::trunc + fs::append)))
    { fs::g_tls_error = fs::error::readonly; return false; }
    if (mode & (fs::lock + fs::unread + fs::append))
    { fs::g_tls_error = fs::error::inval; return false; }
    return true;
}

class stream_file final : public fs::file_base
{
    winrt::agile_ref<IRandomAccessStream> m_stream;
    std::mutex m_mutex;
    u64 m_position = 0;
    bool m_writable;

    u64 read_at_impl(u64 offset, void* output, u64 count)
    {
        auto stream = m_stream.get();
        if (!stream.CanRead()) { fs::g_tls_error = fs::error::acces; return 0; }
        if (offset >= stream.Size()) return 0;
        count = std::min(count, stream.Size() - offset);
        auto input = stream.GetInputStreamAt(offset);
        DataReader reader(input);
        reader.InputStreamOptions(InputStreamOptions::Partial);
        u64 done = 0;
        while (done < count)
        {
            const auto request = static_cast<uint32_t>(std::min<u64>(count - done, 1024 * 1024));
            const auto got = reader.LoadAsync(request).get();
            if (!got) break;
            auto begin = static_cast<uint8_t*>(output) + done;
            reader.ReadBytes(winrt::array_view<uint8_t>(begin, begin + got));
            done += got;
        }
        reader.DetachStream();
        return done;
    }
public:
    stream_file(const IRandomAccessStream& stream, bool writable)
        : m_stream(stream.CloneStream()), m_writable(writable) {}
    fs::stat_t get_stat() override
    { apartment scope; return {false, false, m_writable, m_stream.get().Size()}; }
    fs::native_handle get_handle() override
    { fs::g_tls_error = fs::error::inval; return INVALID_HANDLE_VALUE; }
    u64 read_at(u64 offset, void* output, u64 count) override
    {
        try { apartment scope; std::lock_guard lock(m_mutex); return read_at_impl(offset, output, count); }
        catch (const winrt::hresult_error& ex) { io_error(ex); return 0; }
    }
    u64 read(void* output, u64 count) override
    {
        try
        {
            apartment scope; std::lock_guard lock(m_mutex);
            const auto got = read_at_impl(m_position, output, count);
            m_position += got; return got;
        }
        catch (const winrt::hresult_error& ex) { io_error(ex); return 0; }
    }
    u64 write(const void* input, u64 count) override
    {
        if (!m_writable) { fs::g_tls_error = fs::error::readonly; return 0; }
        try
        {
            apartment scope; std::lock_guard lock(m_mutex);
            auto stream = m_stream.get();
            if (!stream.CanWrite()) { fs::g_tls_error = fs::error::acces; return 0; }
            DataWriter writer(stream.GetOutputStreamAt(m_position));
            u64 done = 0;
            while (done < count)
            {
                const auto chunk = static_cast<uint32_t>(std::min<u64>(count - done, 1024 * 1024));
                const auto begin = static_cast<const uint8_t*>(input) + done;
                writer.WriteBytes(winrt::array_view<const uint8_t>(begin, begin + chunk));
                const auto wrote = writer.StoreAsync().get();
                done += wrote; m_position += wrote;
                if (wrote != chunk) break;
            }
            writer.DetachStream(); return done;
        }
        catch (const winrt::hresult_error& ex) { io_error(ex); return 0; }
    }
    bool trunc(u64 length) override
    {
        if (!m_writable) { fs::g_tls_error = fs::error::readonly; return false; }
        try { apartment scope; std::lock_guard lock(m_mutex); m_stream.get().Size(length); return true; }
        catch (const winrt::hresult_error& ex) { io_error(ex); return false; }
    }
    void sync() override
    {
        try { apartment scope; std::lock_guard lock(m_mutex); if (m_writable) m_stream.get().FlushAsync().get(); }
        catch (const winrt::hresult_error& ex) { io_error(ex); }
    }
    u64 size() override { apartment scope; return m_stream.get().Size(); }
    u64 seek(s64 offset, fs::seek_mode whence) override
    {
        apartment scope; std::lock_guard lock(m_mutex);
        const u64 base = whence == fs::seek_set ? 0 : whence == fs::seek_cur ? m_position : m_stream.get().Size();
        const u64 magnitude = offset < 0 ? u64(-(offset + 1)) + 1 : u64(offset);
        if (whence > fs::seek_end || (offset < 0 && magnitude > base) ||
            (offset >= 0 && magnitude > std::numeric_limits<u64>::max() - base))
        { fs::g_tls_error = fs::error::inval; return umax; }
        return m_position = offset < 0 ? base - magnitude : base + magnitude;
    }
};

class directory final : public fs::dir_base
{
    std::vector<fs::dir_entry> m_entries;
    size_t m_index = 0;
public:
    explicit directory(std::vector<fs::dir_entry> entries) : m_entries(std::move(entries)) {}
    bool read(fs::dir_entry& entry) override
    { if (m_index == m_entries.size()) return false; entry = m_entries[m_index++]; return true; }
    void rewind() override { m_index = 0; }
};

class broker_device final : public fs::device_base
{
    uint32_t m_kind;
    bool m_writable;
    winrt::agile_ref<StorageFolder> m_folder;
    winrt::agile_ref<StorageFile> m_file;
    winrt::agile_ref<IRandomAccessStream> m_stream;
    HANDLE m_handle = INVALID_HANDLE_VALUE;

    std::vector<std::string> parts(const std::string& path)
    {
        std::vector<std::string> result;
        if (!brokered_relative_parts(path, fs_prefix, result)) throw winrt::hresult_access_denied();
        return result;
    }
    StorageFolder folder(const std::vector<std::string>& path, size_t depth)
    {
        auto current = m_folder.get();
        for (size_t i = 0; i < depth; ++i)
        {
            const auto item = current.TryGetItemAsync(winrt::to_hstring(path[i])).get();
            if (!item) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
            if (!item.IsOfType(StorageItemTypes::Folder))
                throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_DIRECTORY));
            current = item.as<StorageFolder>();
        }
        return current;
    }
public:
    broker_device(uint32_t kind, void* abi, bool writable, const std::string& name)
        : m_kind(kind), m_writable(writable)
    {
        fs_prefix += name;
        if (kind == RPCS3_CORE_STORAGE_HANDLE)
        {
            BY_HANDLE_FILE_INFORMATION info{};
            winrt::check_bool(GetFileInformationByHandle(abi, &info));
            if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) throw winrt::hresult_invalid_argument();
            winrt::check_bool(DuplicateHandle(GetCurrentProcess(), abi, GetCurrentProcess(), &m_handle,
                0, FALSE, DUPLICATE_SAME_ACCESS));
            return;
        }
        winrt::Windows::Foundation::IInspectable object{nullptr};
        winrt::copy_from_abi(object, abi);
        if (kind == RPCS3_CORE_STORAGE_FOLDER) m_folder = winrt::agile_ref(object.as<StorageFolder>());
        else if (kind == RPCS3_CORE_STORAGE_FILE) m_file = winrt::agile_ref(object.as<StorageFile>());
        else if (kind == RPCS3_CORE_STORAGE_STREAM) m_stream = winrt::agile_ref(object.as<IRandomAccessStream>());
        else throw winrt::hresult_invalid_argument();
    }
    ~broker_device() override { if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle); }
    bool stat(const std::string& path, fs::stat_t& info) override
    {
        try
        {
            apartment scope; const auto names = parts(path);
            if (names.empty()) { info = {true, false, m_writable}; return true; }
            if (m_kind == RPCS3_CORE_STORAGE_FOLDER)
            {
                auto parent = folder(names, names.size() - 1);
                auto item = parent.TryGetItemAsync(winrt::to_hstring(names.back())).get();
                if (!item) { fs::g_tls_error = fs::error::noent; return false; }
                const auto props = item.GetBasicPropertiesAsync().get();
                info = {item.IsOfType(StorageItemTypes::Folder), false, m_writable, props.Size()};
                info.mtime = props.DateModified().time_since_epoch().count() / 10'000'000 - 11'644'473'600;
                return true;
            }
            if (names != std::vector<std::string>{"content"}) { fs::g_tls_error = fs::error::noent; return false; }
            if (m_kind == RPCS3_CORE_STORAGE_HANDLE)
            {
                LARGE_INTEGER size{}; winrt::check_bool(GetFileSizeEx(m_handle, &size));
                info = {false, false, m_writable, static_cast<u64>(size.QuadPart)};
            }
            else info = {false, false, m_writable, m_kind == RPCS3_CORE_STORAGE_FILE ?
                m_file.get().GetBasicPropertiesAsync().get().Size() : m_stream.get().Size()};
            return true;
        }
        catch (const winrt::hresult_error& ex) { io_error(ex, path); return false; }
    }
    bool statfs(const std::string&, fs::device_stat&) override
    { fs::g_tls_error = fs::error::inval; return false; } // No fabricated capacity.
    std::unique_ptr<fs::file_base> open(const std::string& path, bs_t<fs::open_mode> mode) override
    {
        if (!allowed_mode(mode, m_writable)) return {};
        try
        {
            apartment scope; const auto names = parts(path);
            if (names.empty()) { fs::g_tls_error = fs::error::isdir; return {}; }
            const bool writing = !!(mode & fs::write);
            IRandomAccessStream stream{nullptr};
            if (m_kind == RPCS3_CORE_STORAGE_FOLDER)
            {
                auto parent = folder(names, names.size() - 1);
                StorageFile file{nullptr};
                if (mode & fs::create) file = parent.CreateFileAsync(winrt::to_hstring(names.back()),
                    (mode & fs::excl) ? CreationCollisionOption::FailIfExists : CreationCollisionOption::OpenIfExists).get();
                else
                {
                    const auto item = parent.TryGetItemAsync(winrt::to_hstring(names.back())).get();
                    if (!item) { fs::g_tls_error = fs::error::noent; return {}; }
                    if (!item.IsOfType(StorageItemTypes::File))
                    { fs::g_tls_error = fs::error::isdir; return {}; }
                    file = item.as<StorageFile>();
                }
                stream = file.OpenAsync(writing ? FileAccessMode::ReadWrite : FileAccessMode::Read).get();
            }
            else
            {
                if (names != std::vector<std::string>{"content"}) { fs::g_tls_error = fs::error::noent; return {}; }
                if (mode & (fs::create + fs::excl)) { fs::g_tls_error = fs::error::inval; return {}; }
                if (m_kind == RPCS3_CORE_STORAGE_HANDLE)
                {
                    HANDLE duplicate = INVALID_HANDLE_VALUE;
                    winrt::check_bool(DuplicateHandle(GetCurrentProcess(), m_handle, GetCurrentProcess(),
                        &duplicate, writing ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ, FALSE, 0));
                    auto file = fs::file::from_native_handle(duplicate);
                    if (mode & fs::trunc) file.trunc(0);
                    if (mode & fs::append) file.seek(0, fs::seek_end);
                    return file.release();
                }
                stream = m_kind == RPCS3_CORE_STORAGE_FILE ?
                    m_file.get().OpenAsync(writing ? FileAccessMode::ReadWrite : FileAccessMode::Read).get() : m_stream.get();
            }
            if ((writing && !stream.CanWrite()) || ((mode & fs::read) && !stream.CanRead()))
            { fs::g_tls_error = fs::error::acces; return {}; }
            auto result = std::make_unique<stream_file>(stream, writing);
            if (mode & fs::trunc) if (!result->trunc(0)) return {};
            if (mode & fs::append) result->seek(0, fs::seek_end);
            return result;
        }
        catch (const winrt::hresult_error& ex) { io_error(ex, path); return {}; }
    }
    std::unique_ptr<fs::dir_base> open_dir(const std::string& path) override
    {
        try
        {
            apartment scope; const auto names = parts(path);
            std::vector<fs::dir_entry> entries;
            if (m_kind == RPCS3_CORE_STORAGE_FOLDER)
            {
                const auto root = folder(names, names.size());
                constexpr uint32_t page_size = 128;
                constexpr uint32_t property_batch_size = 16;
                for (uint32_t offset = 0;; offset += page_size)
                {
                    const auto items = root.GetItemsAsync(offset, page_size).get();
                    for (uint32_t begin = 0; begin < items.Size(); begin += property_batch_size)
                    {
                        const auto end = std::min(begin + property_batch_size, items.Size());
                        const auto base = entries.size();
                        using properties_operation = winrt::Windows::Foundation::IAsyncOperation<
                            winrt::Windows::Storage::FileProperties::BasicProperties>;
                        std::vector<std::pair<std::size_t, properties_operation>> pending;
                        pending.reserve(property_batch_size);
                        // Launch a bounded group before waiting. Folder sizes are
                        // unused by recursive size calculation; files retain exact sizes.
                        for (uint32_t index = begin; index < end; ++index)
                        {
                            const auto item = items.GetAt(index);
                            fs::dir_entry entry;
                            entry.name = winrt::to_string(item.Name());
                            entry.is_directory = item.IsOfType(StorageItemTypes::Folder);
                            entry.is_writable = m_writable;
                            if (!entry.is_directory)
                                pending.emplace_back(index - begin, item.GetBasicPropertiesAsync());
                            entries.push_back(std::move(entry));
                        }
                        for (auto& [index, operation] : pending)
                            entries[base + index].size = operation.get().Size();
                    }
                    if (items.Size() < page_size) break;
                }
            }
            else
            {
                if (!names.empty()) { fs::g_tls_error = fs::error::notdir; return {}; }
                fs::dir_entry entry; entry.name = "content";
                if (!stat(fs_prefix + "/content", entry)) return {};
                entries.push_back(std::move(entry));
            }
            return std::make_unique<directory>(std::move(entries));
        }
        catch (const winrt::hresult_error& ex) { io_error(ex, path); return {}; }
    }
    bool create_dir(const std::string& path) override
    {
        if (!m_writable || m_kind != RPCS3_CORE_STORAGE_FOLDER) { fs::g_tls_error = fs::error::readonly; return false; }
        try
        {
            apartment scope; const auto names = parts(path);
            if (names.empty()) { fs::g_tls_error = fs::error::exist; return false; }
            folder(names, names.size() - 1).CreateFolderAsync(winrt::to_hstring(names.back()),
                CreationCollisionOption::FailIfExists).get(); return true;
        }
        catch (const winrt::hresult_error& ex) { io_error(ex, path); return false; }
    }
    bool remove(const std::string& path) override
    {
        if (!m_writable || m_kind != RPCS3_CORE_STORAGE_FOLDER) { fs::g_tls_error = fs::error::readonly; return false; }
        try
        {
            apartment scope; const auto names = parts(path);
            if (names.empty()) { fs::g_tls_error = fs::error::acces; return false; }
            auto item = folder(names, names.size() - 1).GetItemAsync(winrt::to_hstring(names.back())).get();
            if (item.IsOfType(StorageItemTypes::Folder)) { fs::g_tls_error = fs::error::isdir; return false; }
            item.DeleteAsync(StorageDeleteOption::PermanentDelete).get(); return true;
        }
        catch (const winrt::hresult_error& ex) { io_error(ex, path); return false; }
    }
    bool remove_dir(const std::string& path) override
    {
        // WinRT DeleteAsync is recursive. An enumerate-then-delete check is
        // not an atomic rmdir and could delete a concurrently added child.
        (void)path;
        fs::g_tls_error = m_writable ? fs::error::inval : fs::error::readonly;
        return false;
    }
    bool rename(const std::string& from, const std::string& to) override
    {
        if (!m_writable || m_kind != RPCS3_CORE_STORAGE_FOLDER) { fs::g_tls_error = fs::error::readonly; return false; }
        try
        {
            apartment scope; auto old_names = parts(from); auto new_names = parts(to);
            if (old_names.empty() || new_names.empty()) { fs::g_tls_error = fs::error::acces; return false; }
            auto item = folder(old_names, old_names.size() - 1).GetItemAsync(winrt::to_hstring(old_names.back())).get();
            const auto new_name = winrt::to_hstring(new_names.back());
            if (item.IsOfType(StorageItemTypes::File))
                item.as<StorageFile>().MoveAsync(folder(new_names, new_names.size() - 1), new_name,
                    NameCollisionOption::FailIfExists).get();
            else
            {
                old_names.pop_back(); new_names.pop_back();
                if (old_names != new_names) { fs::g_tls_error = fs::error::inval; return false; }
                item.RenameAsync(new_name, NameCollisionOption::FailIfExists).get();
            }
            return true;
        }
        catch (const winrt::hresult_error& ex) { io_error(ex, from); return false; }
    }
    bool trunc(const std::string& path, u64 length) override
    {
        auto file = open(path, fs::write);
        return file && file->trunc(length);
    }
};

std::mutex s_mount_mutex;
std::unordered_set<std::string> s_mount_names;
}

int32_t mount_storage(uint32_t kind, void* abi, const char* name, uint32_t writable,
    char* root, uint32_t capacity, uint32_t* required) try
{
    if (!abi || !name || !*name || !required || writable > 1 || (!root && capacity)) return RPCS3_CORE_INVALID_ARGUMENT;
    if (kind < RPCS3_CORE_STORAGE_FOLDER || kind > RPCS3_CORE_STORAGE_HANDLE) return RPCS3_CORE_INVALID_ARGUMENT;
    const std::string key(name);
    if (key.size() > 64 || key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos)
        return RPCS3_CORE_INVALID_ARGUMENT;
    auto device = stx::make_shared<broker_device>(kind, abi, writable != 0, key);
    const auto prefix = device->fs_prefix + "/";
    *required = static_cast<uint32_t>(prefix.size() + 1);
    if (capacity < *required) return RPCS3_CORE_BUFFER_TOO_SMALL;
    std::lock_guard lock(s_mount_mutex);
    if (s_mount_names.contains(key)) return RPCS3_CORE_INVALID_ARGUMENT;
    s_mount_names.insert(key);
    if (!fs::set_virtual_device(key, device))
    {
        s_mount_names.erase(key);
        return RPCS3_CORE_INTERNAL_ERROR;
    }
    std::memcpy(root, prefix.c_str(), *required);
    return RPCS3_CORE_OK;
}
catch (const winrt::hresult_error& ex)
{
    io_error(ex);
    return RPCS3_CORE_INTERNAL_ERROR;
}

int32_t unmount_storage(const char* name)
{
    if (!name) return RPCS3_CORE_INVALID_ARGUMENT;
    std::lock_guard lock(s_mount_mutex);
    if (!s_mount_names.erase(name)) return RPCS3_CORE_INVALID_ARGUMENT;
    fs::set_virtual_device(name, {});
    return RPCS3_CORE_OK;
}

void clear_storage_mounts()
{
    std::lock_guard lock(s_mount_mutex);
    for (const auto& name : s_mount_names) fs::set_virtual_device(name, {});
    s_mount_names.clear();
}
}
