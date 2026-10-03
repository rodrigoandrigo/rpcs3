#include "host_runtime.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <algorithm>
#include <chrono>

#ifdef RPCS3_UWP
extern void rpcs3_embedded_seh_call(void (*function)(void*), void* context);
#endif

namespace rpcs3::embedded
{
host_runtime::host_runtime() : m_worker([this] { run(); }) {}

host_runtime::~host_runtime()
{
    { std::lock_guard lock(m_mutex); m_exit = true; }
    m_ready.notify_one();
    m_worker.join();
    { std::lock_guard lock(m_log_mutex); m_log_exit = true; }
    m_log_ready.notify_one();
    if (m_log_worker.joinable()) m_log_worker.join();
}

void host_runtime::run()
{
    { std::lock_guard lock(m_mutex); m_owner = std::this_thread::get_id(); }
    for (;;)
    {
        work task;
        {
            std::unique_lock lock(m_mutex);
            m_ready.wait(lock, [this] { return m_exit || !m_work.empty(); });
            if (m_exit && m_work.empty())
            {
                m_worker_done = true;
                lock.unlock();
                wake_dispatcher();
                return;
            }
            task = std::move(m_work.front());
            m_work.pop_front();
            m_busy = true;
        }
        int32_t result = RPCS3_CORE_INTERNAL_ERROR;
        try
        {
#ifdef RPCS3_UWP
            struct invocation { work* task; int32_t* result; } call{&task, &result};
            rpcs3_embedded_seh_call([](void* context)
            {
                auto& value = *static_cast<invocation*>(context);
                *value.result = value.task->function();
            }, &call);
#else
            result = task.function();
#endif
        }
        catch (const std::exception& ex) { error(ex.what()); }
        catch (...) { error("Unknown exception in embedded control task"); }
        if (task.command)
            event(RPCS3_CORE_EVENT_COMMAND_COMPLETE, task.command, result);
        { std::lock_guard lock(m_mutex); m_busy = false; }
        wake_dispatcher();
    }
}

int32_t host_runtime::set_callbacks(const rpcs3_core_callbacks& callbacks)
{
    if (callbacks.struct_size != sizeof(callbacks) || callbacks.api_version != 2)
        return RPCS3_CORE_INVALID_ARGUMENT;
    std::lock_guard lock(m_mutex);
    if (m_pumping) return RPCS3_CORE_BUSY;
    if (m_dispatcher != std::thread::id{} && m_dispatcher != std::this_thread::get_id())
        return RPCS3_CORE_WRONG_THREAD;
    m_dispatcher = std::this_thread::get_id();
    m_callbacks = callbacks;
    return RPCS3_CORE_OK;
}

int32_t host_runtime::submit(uint32_t command, std::function<int32_t()> function)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_exit || m_work.size() >= 128) return RPCS3_CORE_BUSY;
        m_work.push_back({command, std::move(function)});
    }
    m_ready.notify_one();
    return RPCS3_CORE_OK;
}

bool host_runtime::is_owner() const
{
    std::lock_guard lock(m_mutex);
    return m_owner == std::this_thread::get_id();
}

bool host_runtime::idle()
{
    std::lock_guard lock(m_mutex);
    return !m_busy && m_work.empty() && !m_pumping;
}

bool host_runtime::prepare_release()
{
    {
        std::lock_guard lock(m_mutex);
        if (m_busy || !m_work.empty() || m_pumping) return false;
        m_exit = true;
    }
    { std::lock_guard lock(m_log_mutex); m_log_exit = true; }
    m_ready.notify_one();
    m_log_ready.notify_one();
    return m_worker_done && m_log_done;
}

void host_runtime::service_internal(uint32_t wait_ms)
{
    if (!is_owner()) throw std::runtime_error("Internal callbacks require control ownership");
    work task;
    {
        std::unique_lock lock(m_mutex);
        auto find_internal = [this]
        { return std::find_if(m_work.begin(), m_work.end(), [](const work& item) { return item.command == 0; }); };
        m_ready.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] { return find_internal() != m_work.end(); });
        auto found = find_internal();
        if (found == m_work.end()) return;
        task = std::move(*found);
        m_work.erase(found);
    }
    // Nested servicing is only for core callbacks, never another host command.
    task.function();
}

int32_t host_runtime::pump(uint32_t limit)
{
    rpcs3_core_callbacks callbacks;
    {
        std::lock_guard lock(m_mutex);
        if (m_dispatcher != std::this_thread::get_id()) return RPCS3_CORE_WRONG_THREAD;
        if (m_pumping) return RPCS3_CORE_BUSY;
        m_pumping = true;
        m_wake_pending = false;
        callbacks = m_callbacks;
    }
    int32_t result = RPCS3_CORE_OK;
    try
    {
        for (uint32_t i = 0; i < limit; ++i)
        {
            notification item;
            {
                std::lock_guard lock(m_mutex);
                if (m_events.empty()) break;
                item = std::move(m_events.front());
                m_events.pop_front();
            }
            if (!item.type && callbacks.on_log)
                callbacks.on_log(callbacks.user, item.level, item.timestamp,
                    item.channel.c_str(), item.text.c_str());
            else if (item.type && callbacks.on_event)
                callbacks.on_event(callbacks.user, item.type, item.command,
                    item.result, item.text.c_str());
        }
    }
    catch (...) { result = RPCS3_CORE_INTERNAL_ERROR; }
    { std::lock_guard lock(m_mutex); m_pumping = false; }
    return result;
}

void host_runtime::event(uint32_t type, uint32_t command, int32_t result, std::string text)
{
    {
        std::lock_guard lock(m_mutex);
        m_events.push_back({type, command, 0, result, 0, {}, std::move(text)});
    }
    wake_dispatcher();
}

void host_runtime::wake_dispatcher()
{
    rpcs3_core_callbacks callbacks;
    {
        std::lock_guard lock(m_mutex);
        if (m_wake_pending || !m_callbacks.on_wake) return;
        m_wake_pending = true;
        callbacks = m_callbacks;
    }
    try { callbacks.on_wake(callbacks.user); }
    catch (...) { std::lock_guard lock(m_mutex); m_error = "Host wake callback threw"; }
}

void host_runtime::error(std::string text)
{
    { std::lock_guard lock(m_mutex); m_error = text; }
    log(2, 0, "Embedding", text);
    event(RPCS3_CORE_EVENT_ERROR, 0, RPCS3_CORE_INTERNAL_ERROR, std::move(text));
}

std::string host_runtime::last_error()
{
    std::lock_guard lock(m_mutex);
    return m_error;
}

void host_runtime::open_log(const std::string& path)
{
    if (m_log_worker.joinable()) throw std::runtime_error("Log already initialized");
    std::ofstream file(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::app | std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open embedded RPCS3 log: " + path);
    { std::lock_guard lock(m_mutex); m_log_path = path; }
    m_log_done = false;
    m_log_worker = std::thread([this, file = std::move(file)]() mutable
    {
        uint64_t reported_drops = 0;
        for (;;)
        {
            std::deque<std::string> lines;
            bool done;
            {
                std::unique_lock lock(m_log_mutex);
                m_log_ready.wait(lock, [this] { return m_log_exit || !m_log_lines.empty(); });
                lines.swap(m_log_lines);
                done = m_log_exit;
            }
            const auto drops = m_dropped_file.load();
            if (drops != reported_drops)
            {
                file << "[WARNING] Embedding: file log overload dropped " << (drops - reported_drops) << " records\n";
                reported_drops = drops;
            }
            for (const auto& line : lines) file << line;
            file.flush();
            if (!file)
            {
                { std::lock_guard lock(m_mutex); m_error = "Embedded RPCS3 log write failed"; }
                event(RPCS3_CORE_EVENT_ERROR, 0, RPCS3_CORE_INTERNAL_ERROR,
                    "Embedded RPCS3 log write failed");
                m_log_done = true;
                wake_dispatcher();
                return;
            }
            if (done)
            {
                file.close();
                m_log_done = true;
                wake_dispatcher();
                return;
            }
        }
    });
}

std::string host_runtime::log_path()
{
    std::lock_guard lock(m_mutex);
    return m_log_path;
}

void host_runtime::log(uint32_t level, uint64_t timestamp, std::string channel, std::string text)
{
    {
        std::lock_guard lock(m_log_mutex);
        if (m_log_lines.size() >= 8192) { m_log_lines.pop_front(); ++m_dropped_file; }
        m_log_lines.push_back(std::to_string(timestamp) + " [" + std::to_string(level) +
            "] " + channel + ": " + text + "\n");
    }
    m_log_ready.notify_one();
    {
        std::lock_guard lock(m_mutex);
        // Bound callback backlog independently of file logging. Never discard
        // a command completion to make room for a low-priority log record.
        if (m_events.size() < 4096)
            m_events.push_back({0, 0, level, 0, timestamp, std::move(channel), std::move(text)});
        else ++m_dropped_callbacks;
    }
    wake_dispatcher();
}
}
