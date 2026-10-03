#pragma once
#include "core_api.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace rpcs3::embedded
{
// Independent of Qt, WinRT and the emulator; exercised with a fake backend.
class host_runtime
{
    struct work { uint32_t command; std::function<int32_t()> function; };
    struct notification
    {
        uint32_t type{}, command{}, level{};
        int32_t result{};
        uint64_t timestamp{};
        std::string channel, text;
    };
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<work> m_work;
    std::deque<notification> m_events;
    std::thread m_worker;
    std::thread::id m_owner, m_dispatcher;
    bool m_exit = false, m_busy = false, m_pumping = false;
    bool m_wake_pending = false;
    rpcs3_core_callbacks m_callbacks{};
    std::string m_error, m_log_path;
    std::mutex m_log_mutex;
    std::condition_variable m_log_ready;
    std::deque<std::string> m_log_lines;
    std::thread m_log_worker;
    bool m_log_exit = false;
    std::atomic<bool> m_worker_done{false}, m_log_done{true};
    std::atomic<uint64_t> m_dropped_callbacks{0}, m_dropped_file{0};
    void run();
    void wake_dispatcher();
public:
    host_runtime();
    ~host_runtime();
    host_runtime(const host_runtime&) = delete;
    int32_t set_callbacks(const rpcs3_core_callbacks& callbacks);
    int32_t submit(uint32_t command, std::function<int32_t()> function);
    bool is_owner() const;
    bool idle();
    bool prepare_release();
    void service_internal(uint32_t wait_ms);
    int32_t pump(uint32_t limit = 128);
    void event(uint32_t type, uint32_t command, int32_t result, std::string text = {});
    void error(std::string text);
    std::string last_error();
    void open_log(const std::string& path);
    std::string log_path();
    uint64_t dropped_callbacks() const { return m_dropped_callbacks.load(); }
    uint64_t dropped_file() const { return m_dropped_file.load(); }
    void log(uint32_t level, uint64_t timestamp, std::string channel, std::string text);
};
}
