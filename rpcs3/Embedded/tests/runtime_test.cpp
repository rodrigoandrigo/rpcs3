#include "../host_runtime.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace rpcs3::embedded;
using namespace std::chrono_literals;

static void require(bool condition, const char* reason)
{
    if (!condition) throw std::runtime_error(reason);
}

struct capture
{
    host_runtime* host;
    std::thread::id ui = std::this_thread::get_id();
    std::vector<int32_t> results;
    size_t logs = 0, errors = 0;
    bool wrong_thread = false;
    int32_t recursive_result = 0;
};

static void on_event(void* user, uint32_t type, uint32_t, int32_t result, const char*)
{
    auto& target = *static_cast<capture*>(user);
    target.wrong_thread |= target.ui != std::this_thread::get_id();
    if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE) target.results.push_back(result);
    if (type == RPCS3_CORE_EVENT_ERROR) ++target.errors;
    target.recursive_result = target.host->pump();
}

static void on_log(void* user, uint32_t, uint64_t, const char*, const char*)
{
    auto& target = *static_cast<capture*>(user);
    target.wrong_thread |= target.ui != std::this_thread::get_id();
    ++target.logs;
}

int main(int argc, char** argv) try
{
    require(argc == 2, "Pass an explicit test log path");
    const std::string log_utf8(argv[1]);
    const std::filesystem::path log_path(std::u8string(log_utf8.begin(), log_utf8.end()));
    {
        host_runtime host;
        capture target{&host};
        rpcs3_core_callbacks callbacks{sizeof(callbacks), 2, &target, on_log, on_event};
        require(host.set_callbacks(callbacks) == 0, "Register callbacks");
        auto invalid = callbacks;
        invalid.api_version = 1;
        require(host.set_callbacks(invalid) == RPCS3_CORE_INVALID_ARGUMENT, "Reject incompatible ABI");
        require(std::async(std::launch::async, [&] { return host.pump(); }).get() ==
            RPCS3_CORE_WRONG_THREAD, "Reject pump on wrong thread");

        std::promise<void> entered, unblock;
        auto gate = unblock.get_future().share();
        require(host.submit(1, [&]
        {
            require(host.is_owner(), "Worker ownership");
            host.open_log(argv[1]);
            entered.set_value();
            gate.wait();
            host.log(6, 42, "Test", "UTF-8 log: teste");
            return 17;
        }) == 0, "Queue command");
        require(entered.get_future().wait_for(5s) == std::future_status::ready, "Worker started");
        require(!host.idle(), "Release must observe busy worker");
        const auto start = std::chrono::steady_clock::now();
        require(host.pump() == 0, "Pump while control worker is blocked");
        require(std::chrono::steady_clock::now() - start < 100ms, "UI pump must not wait for worker");
        require(host.submit(2, []() -> int32_t { throw std::runtime_error("Injected failure"); }) == 0,
            "Queue failure");
        require(host.submit(3, [] { return 0; }) == 0, "Queue recovery probe");
        unblock.set_value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while ((!host.idle() || target.results.size() < 3) && std::chrono::steady_clock::now() < deadline)
        {
            host.pump();
            std::this_thread::sleep_for(1ms);
        }
        host.pump();
        require(target.results == std::vector<int32_t>({17, RPCS3_CORE_INTERNAL_ERROR, 0}),
            "Ordered completion and exception containment");
        require(target.errors == 1 && target.logs >= 2, "Error and log callbacks");
        require(!target.wrong_thread, "Callbacks run on UI dispatcher only");
        require(target.recursive_result == RPCS3_CORE_BUSY, "Reject recursive pump");
        require(host.last_error() == "Injected failure", "Preserve last error");
        require(host.log_path() == argv[1], "Report log path");
        for (int i = 0; i < 5000; ++i) host.log(7, i, "Flood", "bounded log test");
        require(host.dropped_callbacks() >= 904, "Account for bounded callback backlog losses");
        require(std::async(std::launch::async, [&] { return host.pump(); }).get() ==
            RPCS3_CORE_WRONG_THREAD, "Affinity still enforced after commands");
        const auto release_deadline = std::chrono::steady_clock::now() + 5s;
        while (!host.prepare_release() && std::chrono::steady_clock::now() < release_deadline)
            std::this_thread::sleep_for(1ms);
        require(host.prepare_release(), "Release waits for control and log workers to finish");
        require(host.submit(4, [] { return 0; }) == RPCS3_CORE_BUSY, "Reject commands after release starts");
    }
    std::ifstream file(log_path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(file)), {});
    require(content.find("UTF-8 log: teste") != std::string::npos, "Flush file log");
    require(content.find("Injected failure") != std::string::npos, "Persist error");
    std::cout << "PASS: async commands, UI pump, callback affinity, exception containment, file log, release, overflow accounting\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
