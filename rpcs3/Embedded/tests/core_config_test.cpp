#include <Windows.h>
#include "../core_api.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// Run against a freshly built DLL and an isolated writable state directory.
// This checks configuration behavior, not AppContainer or guest execution.
struct entry
{
    unsigned type = 0, flags = 0;
    std::string value, defaults, group, choices, minimum, maximum, restriction;
};
struct events
{
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    unsigned command = 0;
    int result = 999;
    bool complete = false;
};

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4) return 2;
    if (argc == 4) AddDllDirectory(std::filesystem::absolute(argv[3]).c_str());
    const auto dllPath = std::filesystem::absolute(argv[1]);
    const auto statePath = std::filesystem::absolute(argv[2]);
    if (std::filesystem::exists(statePath)) {
        std::cerr << "Use a new isolated state directory\n"; return 2;
    }
    std::filesystem::create_directories(statePath);
    HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) { std::cerr << "LoadLibrary: " << GetLastError() << '\n'; return 2; }
#define API(name) auto name = reinterpret_cast<decltype(&::name)>(GetProcAddress(module, #name)); if (!name) return 2
    API(rpcs3_core_set_callbacks); API(rpcs3_core_initialize); API(rpcs3_core_pump);
    API(rpcs3_core_enumerate_config); API(rpcs3_core_set_config);
    API(rpcs3_core_reset_config); API(rpcs3_core_save_settings);
    API(rpcs3_core_shutdown); API(rpcs3_core_release);
    events state;
    const rpcs3_core_callbacks callbacks{sizeof(callbacks), 2, &state, nullptr,
        [](void* user, unsigned type, unsigned command, int result, const char* message) {
            auto& s = *static_cast<events*>(user);
            if (type == RPCS3_CORE_EVENT_COMMAND_COMPLETE && command == s.command) {
                s.result = result; s.complete = true;
            }
            if (type == RPCS3_CORE_EVENT_ERROR) std::cerr << "Core: " << message << '\n';
        }, [](void* user) { SetEvent(static_cast<events*>(user)->wake); }};
    if (rpcs3_core_set_callbacks(&callbacks) != 0) return 2;
    auto wait = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!state.complete && std::chrono::steady_clock::now() < deadline) {
            rpcs3_core_pump();
            if (!state.complete) WaitForSingleObject(state.wake, 20);
        }
        return state.complete ? state.result : 999;
    };
    auto prepare = [&](unsigned command) { state.command = command; state.complete = false; state.result = 999; };
    prepare(RPCS3_CORE_COMMAND_INITIALIZE);
    if (rpcs3_core_initialize(statePath.string().c_str()) != 0 || wait() != 0) return 3;
    std::map<std::string, entry> entries;
    auto snapshot = [&] {
        entries.clear();
        return rpcs3_core_enumerate_config([](void* user, const rpcs3_core_config_entry* e) {
            auto& out = *static_cast<std::map<std::string, entry>*>(user);
            out.emplace(e->path, entry{e->type, e->flags, e->value, e->default_value,
                e->group, e->enum_values, e->minimum_value, e->maximum_value, e->restriction});
        }, &entries);
    };
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "PASS " : "FAIL ") << label << '\n'; failures += !ok;
    };
    check(snapshot() == 0 && entries.size() > 200, "complete configuration snapshot");
    for (const auto& [path, e] : entries) {
        if (e.group.empty() || (e.type == RPCS3_CORE_CONFIG_ENUM && e.choices.empty()) ||
            ((e.flags & RPCS3_CORE_CONFIG_READ_ONLY) && e.restriction.empty())) {
            std::cerr << "Invalid metadata: " << path << '\n'; ++failures;
        }
        std::cout << "ENTRY " << path << '\n';
    }
    check(entries.at("Input/Output/Camera flip").group == "Input/Output", "slash-containing group");
    check(entries.contains("Mounts//dev_hdd0/") && entries.contains("IPC/IPC Port"), "VFS and IPC visibility");
    auto set = [&](const char* path, const std::string& value) {
        prepare(RPCS3_CORE_COMMAND_SET_CONFIG);
        const auto result = rpcs3_core_set_config(path, value.c_str());
        return result == 0 ? wait() : result;
    };
    check(set("Input/Output/Camera flip", entries.at("Input/Output/Camera flip").value) == 0,
        "resolve slash-containing group");
    check(set("VFS/Enable /host_root/", "false") == 0, "resolve slash-containing leaf");
    check(set("Core/PPU Threads", "9999") == RPCS3_CORE_INVALID_ARGUMENT, "reject out-of-range number");
    check(set("Core/Libraries Control", "[libfoo.sprx, libbar.sprx]") == 0, "apply collection");
    snapshot();
    const auto libraries = entries.at("Core/Libraries Control").value;
    check(libraries.find("libfoo.sprx") != std::string::npos, "collection serialization");
    check(set("Core/Libraries Control", "invalid-scalar") == RPCS3_CORE_INVALID_ARGUMENT, "reject malformed collection");
    snapshot();
    check(entries.at("Core/Libraries Control").value == libraries, "invalid input preserves previous collection");
    check(set("Log", "{SYS: Notice}") == 0, "apply log map");
    check(set("Log", "{SYS: InvalidLevel}") == RPCS3_CORE_INVALID_ARGUMENT, "reject invalid log level");
    check(set("Audio/Renderer", "Cubeb") == RPCS3_CORE_UNSUPPORTED_RENDERER, "reject unavailable backend");
    check(entries.at("Audio/Enable Buffering").value == "false", "Null audio buffering disabled");
    check(set("Audio/Enable Buffering", "true") != RPCS3_CORE_OK, "reject unsupported audio buffering");
    const std::string longValue(9000, 'x');
    check(set("Core/Use LLVM CPU", longValue) == 0, "apply long text");
    snapshot();
    check(entries.at("Core/Use LLVM CPU").value == longValue, "long text is not truncated");
    prepare(RPCS3_CORE_COMMAND_RESET_CONFIG);
    check(rpcs3_core_reset_config("Input/Output") == 0 && wait() == 0, "reset slash-containing group");
    prepare(RPCS3_CORE_COMMAND_RESET_CONFIG);
    check(rpcs3_core_reset_config("") == 0 && wait() == 0, "reset whole tree");
    snapshot();
    check(entries.at("Audio/Renderer").value == entries.at("Audio/Renderer").defaults &&
        entries.at("Input/Output/Mouse").value == entries.at("Input/Output/Mouse").defaults,
        "reset preserves UWP backend policy");
    std::filesystem::rename(statePath / "config", statePath / "config.saved");
    std::ofstream(statePath / "config") << "Intentional I/O failure fixture";
    prepare(RPCS3_CORE_COMMAND_SAVE_SETTINGS);
    check(rpcs3_core_save_settings() == 0 && wait() == RPCS3_CORE_IO_ERROR,
        "report persistence failure");
    const auto beforeFailure = entries.at("Core/PPU Threads").value;
    check(set("Core/PPU Threads", "3") == RPCS3_CORE_IO_ERROR, "reject unsaved mutation");
    snapshot();
    check(entries.at("Core/PPU Threads").value == beforeFailure, "persistence failure rolls back mutation");
    std::filesystem::rename(statePath / "config", statePath / "config.blocked");
    std::filesystem::rename(statePath / "config.saved", statePath / "config");
    prepare(RPCS3_CORE_COMMAND_SAVE_SETTINGS);
    check(rpcs3_core_save_settings() == 0 && wait() == 0 &&
        std::filesystem::exists(statePath / "config/config.yml"), "persist configuration");
    std::cout << "Configuration entries: " << entries.size() << '\n';
    prepare(RPCS3_CORE_COMMAND_SHUTDOWN);
    check(rpcs3_core_shutdown() == 0 && wait() == 0, "shutdown");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int release = RPCS3_CORE_BUSY;
    while (release == RPCS3_CORE_BUSY && std::chrono::steady_clock::now() < deadline) {
        release = rpcs3_core_release();
        if (release == RPCS3_CORE_BUSY) WaitForSingleObject(state.wake, 20);
    }
    check(release == 0, "release");
    if (release == 0) FreeLibrary(module);
    CloseHandle(state.wake);
    return failures ? 1 : 0;
}
