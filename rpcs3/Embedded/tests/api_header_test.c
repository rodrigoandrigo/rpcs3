#include "../core_api.h"

/* Compile-only probe: the public interface is consumable as C, without STL,
 * Qt, C++/CX or WinRT headers. It does not assert a successful DLL link. */
static void receive_log(void* user, uint32_t level, uint64_t time,
    const char* channel, const char* text)
{
    (void)user; (void)level; (void)time; (void)channel; (void)text;
}

struct rpcs3_core_callbacks make_c_callbacks(void)
{
    struct rpcs3_core_callbacks callbacks = {0};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.api_version = 2;
    callbacks.on_log = receive_log;
    return callbacks;
}

/* Keep administrative ABI additions C-compatible and visible to consumers. */
int32_t (*const install_package_api)(const char*) = rpcs3_core_install_package;
int32_t (*const install_firmware_api)(const char*) = rpcs3_core_install_firmware;
