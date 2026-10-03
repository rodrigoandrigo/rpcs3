#pragma once
#include <stdint.h>

#if defined(RPCS3_CORE_EXPORTS)
#define RPCS3_CORE_API __declspec(dllexport)
#else
#define RPCS3_CORE_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum rpcs3_core_result
{
    RPCS3_CORE_OK = 0,
    RPCS3_CORE_INVALID_ARGUMENT = -1,
    RPCS3_CORE_NOT_INITIALIZED = -2,
    RPCS3_CORE_WRONG_THREAD = -3,
    RPCS3_CORE_ALREADY_INITIALIZED = -4,
    RPCS3_CORE_UNSUPPORTED_RENDERER = -5,
    RPCS3_CORE_INTERNAL_ERROR = -6,
    RPCS3_CORE_BUSY = -7,
    RPCS3_CORE_FAULTED = -8,
    RPCS3_CORE_BUFFER_TOO_SMALL = -9,
    RPCS3_CORE_IO_ERROR = -10
};

enum rpcs3_core_event_type
{
    RPCS3_CORE_EVENT_COMMAND_COMPLETE = 1,
    RPCS3_CORE_EVENT_STATE_CHANGED = 2,
    RPCS3_CORE_EVENT_ERROR = 3,
    RPCS3_CORE_EVENT_QUIT_REQUESTED = 4
};

enum rpcs3_core_command
{
    RPCS3_CORE_COMMAND_INITIALIZE = 1,
    RPCS3_CORE_COMMAND_BOOT = 2,
    RPCS3_CORE_COMMAND_PAUSE = 3,
    RPCS3_CORE_COMMAND_RESUME = 4,
    RPCS3_CORE_COMMAND_STOP = 5,
    RPCS3_CORE_COMMAND_SHUTDOWN = 6,
    RPCS3_CORE_COMMAND_EJECT_DISC = 7,
    RPCS3_CORE_COMMAND_INSERT_DISC = 8,
    RPCS3_CORE_COMMAND_SAVE_SETTINGS = 9,
    RPCS3_CORE_COMMAND_SET_CONFIG = 10,
    RPCS3_CORE_COMMAND_RESET_CONFIG = 11,
    RPCS3_CORE_COMMAND_INSTALL_PACKAGE = 12,
    RPCS3_CORE_COMMAND_INSTALL_FIRMWARE = 13
};

/* Callback strings are UTF-8, borrowed only until the callback returns.
 * Callbacks run only from pump() on the registering host thread. They must not
 * throw, block, unload the DLL, or recursively pump/release it. */
typedef void (*rpcs3_core_log_callback)(void* user, uint32_t level,
    uint64_t timestamp_us, const char* channel, const char* text);
typedef void (*rpcs3_core_event_callback)(void* user, uint32_t type,
    uint32_t command, int32_t result, const char* message);
/* Optional wake-only callback: may run on any producer thread. It may post to
 * CoreDispatcher, but must not touch UI, call back into the DLL or throw. */
typedef void (*rpcs3_core_wake_callback)(void* user);
struct rpcs3_core_callbacks
{
    uint32_t struct_size;
    uint32_t api_version;
    void* user;
    rpcs3_core_log_callback on_log;
    rpcs3_core_event_callback on_event;
    rpcs3_core_wake_callback on_wake;
};

/* API v2: lifecycle calls return OK when QUEUED, not when completed.
 * Completion and positive game_boot_result values arrive via on_event.
 * One control worker owns Emu; the UI only submits commands and pumps events.
 * Register callbacks before initialize. state_root must be an absolute UTF-8
 * writable directory under the host's ApplicationData LocalFolder.
 * shutdown is asynchronous; keep pumping until its completion, then release.
 * release returns BUSY instead of waiting for active work. No forced unload.
 * Callbacks may submit commands. Concurrent pumps are not supported.
 * The initial build supports the Null renderer for integration diagnostics.
 * It must not advertise D3D12 until the RSX port has passed validation. */
RPCS3_CORE_API uint32_t rpcs3_core_api_version(void);
RPCS3_CORE_API const char* rpcs3_core_version(void);
RPCS3_CORE_API int32_t rpcs3_core_set_callbacks(const struct rpcs3_core_callbacks* callbacks);
RPCS3_CORE_API int32_t rpcs3_core_initialize(const char* state_root_utf8);
RPCS3_CORE_API int32_t rpcs3_core_boot(const char* path_utf8);
RPCS3_CORE_API int32_t rpcs3_core_pump(void);
RPCS3_CORE_API int32_t rpcs3_core_pause(void);
RPCS3_CORE_API int32_t rpcs3_core_resume(void);
RPCS3_CORE_API int32_t rpcs3_core_stop(void);
RPCS3_CORE_API int32_t rpcs3_core_shutdown(void);
RPCS3_CORE_API int32_t rpcs3_core_release(void);
RPCS3_CORE_API int32_t rpcs3_core_eject_disc(void);
RPCS3_CORE_API int32_t rpcs3_core_insert_disc(const char* path_utf8);
RPCS3_CORE_API int32_t rpcs3_core_save_settings(void);
/* Installs a PS3 PKG from a native or broker-mounted UTF-8 path. The command
 * is serialized on the core worker and reports completion through on_event. */
RPCS3_CORE_API int32_t rpcs3_core_install_package(const char* path_utf8);
RPCS3_CORE_API int32_t rpcs3_core_install_firmware(const char* path_utf8);
/* Copies NUL-terminated text; required includes the terminator. */
RPCS3_CORE_API int32_t rpcs3_core_last_error(char* output, uint32_t capacity, uint32_t* required);
RPCS3_CORE_API int32_t rpcs3_core_log_path(char* output, uint32_t capacity, uint32_t* required);
RPCS3_CORE_API int32_t rpcs3_core_log_stats(uint64_t* dropped_callback_records, uint64_t* dropped_file_records);
RPCS3_CORE_API uint32_t rpcs3_core_state(void);

enum rpcs3_core_config_type
{
    RPCS3_CORE_CONFIG_BOOL = 1,
    RPCS3_CORE_CONFIG_ENUM = 2,
    RPCS3_CORE_CONFIG_NUMBER = 3,
    RPCS3_CORE_CONFIG_TEXT = 4,
    RPCS3_CORE_CONFIG_COLLECTION = 5
};

enum rpcs3_core_config_flags
{
    RPCS3_CORE_CONFIG_DYNAMIC = 1u << 0,
    RPCS3_CORE_CONFIG_READ_ONLY = 1u << 1
};

/* Complete Qt-independent view of the emulator configuration tree. Strings
 * are UTF-8 and borrowed only for the duration of the callback. enum_values
 * uses ASCII unit-separator (0x1f) between choices. The snapshot may be read
 * on the UI thread; mutations are serialized onto the core worker. */
struct rpcs3_core_config_entry
{
    uint32_t struct_size;
    uint32_t type;
    uint32_t flags;
    const char* path;
    const char* name;
    const char* value;
    const char* default_value;
    const char* enum_values;
    /* Appended metadata: inspect struct_size before using these fields. */
    const char* group;
    const char* minimum_value;
    const char* maximum_value;
    const char* restriction;
};
typedef void (*rpcs3_core_config_callback)(void* user,
    const struct rpcs3_core_config_entry* entry);
RPCS3_CORE_API int32_t rpcs3_core_enumerate_config(
    rpcs3_core_config_callback callback, void* user);
RPCS3_CORE_API int32_t rpcs3_core_set_config(const char* path_utf8,
    const char* value_utf8);
/* Empty prefix resets the entire tree. Otherwise it names a node or leaf. */
RPCS3_CORE_API int32_t rpcs3_core_reset_config(const char* prefix_utf8);

enum rpcs3_core_storage_kind
{
    RPCS3_CORE_STORAGE_FOLDER = 1,
    RPCS3_CORE_STORAGE_FILE = 2,
    RPCS3_CORE_STORAGE_STREAM = 3,
    RPCS3_CORE_STORAGE_HANDLE = 4
};
/* Additive API v2 extension. storage_abi is a borrowed IInspectable* for the
 * first three kinds (winrt::get_abi), or a borrowed file HANDLE for HANDLE.
 * The DLL retains an agile COM reference / duplicates the handle; the caller
 * retains ownership. Mount while fully stopped, after initialize completion.
 * Returns a synthetic VFS root, never the physical StorageItem.Path.
 * Single files/streams/handles appear as <root>/content. Read-only by default;
 * writable=1 requests access, never grants permission the object does not have.
 * Mount/unmount are synchronous metadata operations, not UI file IO. */
RPCS3_CORE_API int32_t rpcs3_core_mount_storage(uint32_t kind, void* storage_abi,
    const char* mount_name_utf8, uint32_t writable, char* root_utf8,
    uint32_t capacity, uint32_t* required);
RPCS3_CORE_API int32_t rpcs3_core_unmount_storage(const char* mount_name_utf8);

/* Additive API v2 experimental D3D12 embedding extension. Attach the frontend's
 * ID3D12Device* and DIRECT ID3D12CommandQueue* while stopped, after initialize.
 * Both are borrowed and retained by the DLL. Builds without the experimental
 * renderer return UNSUPPORTED_RENDERER. The host must use this same queue for
 * all sampling of acquired frames: publication follows producer submission.
 * acquire returns an AddRef'd ID3D12Resource* (immutable RGBA8, PIXEL_SHADER_RESOURCE).
 * The host owns that reference and must retain it AND its SRV until its own GPU
 * fence completes. BUSY means no frame yet, not an error. Never create a second
 * swapchain or call Present from the core. Stop before releasing host graphics. */
RPCS3_CORE_API int32_t rpcs3_core_attach_d3d12(void* device, void* direct_queue);
RPCS3_CORE_API int32_t rpcs3_core_acquire_d3d12_frame(void** resource, uint64_t* serial);

#ifdef __cplusplus
}
#endif
