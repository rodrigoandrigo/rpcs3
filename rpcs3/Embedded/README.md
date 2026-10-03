# RPCS3 embedding / UWP facade

Status: partial port, not a working UWP emulator. The Qt-free WindowsStore DLL
now links, and its static API/import audit passed. The RSX D3D12 backend and
AppContainer/game execution remain unvalidated; see `RPCS3-UWP/PORT-STATUS.md`.

## Public interface

`core_api.h` is the exported **C ABI**, version 2. Do not export `Emulator`, STL
containers, Qt objects or allocator ownership to the host. This facade exposes
version, initialization, boot, pause/resume, stop, final shutdown/release, disc
insertion/ejection, settings persistence, state, last-error and log-path queries,
callback registration and notification pumping. All declarations carry the DLL
export/import attribute. Actual PE exports must be audited after a successful
DLL link; compiling an object is not that audit.

Other internal/public C++ methods (savestates, firmware and package installation,
debugger, complete configuration, graphics presentation and emulator dialogs)
are not yet exposed as supported embedding operations. Default builds retain
Null graphics/audio. Build-Core.ps1 -ExperimentalD3D12 additionally links the
adapted legacy RSX renderer; audio remains Null. This is not a claim of complete
D3D12 compatibility or audio emulation.

The additive v2 APIs rpcs3_core_attach_d3d12 and
rpcs3_core_acquire_d3d12_frame implement experimental shared presentation.
Attach the frontend's device/DIRECT queue after initialization, while stopped.
The DLL retains both; acquired frames are AddRef'd immutable RGBA8 textures in
PIXEL_SHADER_RESOURCE state. Sample them only on the same queue, and retain
each resource AND its SRV until the host GPU fence completes. Publication
follows producer submission, so no UI-thread CPU wait is needed for the core.
The connected frontend implements per-swapchain-slot lifetime and an ImGui
video window. Only the frontend owns presentation and resize. Without the
experimental option, attachment returns UNSUPPORTED_RENDERER.
See Emu/RSX/D3D12/PORTING.md for remaining renderer limitations. Native WARP
component tests, successful DLL/host links and static API audit passed; an
installed AppContainer app, PS3 game and Xbox were not exercised.

Lifecycle calls return `RPCS3_CORE_OK` when a command is accepted into the queue,
not when it finishes. `on_event(COMMAND_COMPLETE, command, result, ...)` carries
the actual result, including positive RPCS3 `game_boot_result` codes. This is a
breaking change from the initial synchronous facade. Commands are FIFO; an
initialization failure prevents subsequent boot operations. Do not infer launch
success from the immediate `boot()` return value.

## Thread and lifecycle contract

1. Register callbacks on the UI/dispatcher thread before initialization. Their
   user pointer must remain valid through successful release; callback changes
   after initialization are rejected.
2. Pass an absolute UTF-8 path beneath the host's
   `ApplicationData::Current().LocalFolder()` to `initialize()`. The DLL does not
   independently acquire arbitrary folder permissions or assume a console.
3. Submit commands. One control worker owns `Emu`, while normal emulation threads
   retain their existing execution model. Qt-style main-thread callbacks are
   marshalled to the control worker, not executed by the UI pump.
4. Call `pump()` on the registering thread. It dispatches at most 128 queued
   notifications per call and never executes boot or a thread join. Log/event
   callbacks must be short, nonthrowing and nonrecursive. They may enqueue a
   command. A single slow host callback can still delay the UI.
5. The optional `on_wake` callback is different: it can run on a producer thread
   and may only post a wake to an agile dispatcher. It must not access UI or
   reenter the DLL. Wakes are coalesced. The native host uses CoreDispatcher so
   hidden/closing UI can wait for events without polling a busy loop.
6. `stop()` ends content but keeps the runtime initialized. `shutdown()` is
   terminal. The control worker services internal callbacks while waiting for
   **fully stopped** state, with a 30-second watchdog. No arbitrary host command
   is executed recursively during that wait.
7. Continue pumping until shutdown completion, then poll `release()`. It returns
   `BUSY` until control and file-writer workers have finished, rather than joining
   active work on the UI thread. Do not unload the DLL before successful release.
   Reinitialization after final shutdown is not supported.

The host owns window lifetime, dispatcher, presentation, package lifecycle and
the decision to exit its own application. The core's quit callback emits a
request; it does not execute the desktop teardown/exit callback.

## Logging

The embedding log listener sends native RPCS3 records to an asynchronous file
writer and a UI notification queue. The file is `<state_root>/RPCS3.log`, in
append mode, including severity, timestamp and channel. Early RPCS3 records are
replayed through `logs::set_init`. Open/write failures are reported as errors,
not only printed to stderr. File flushing runs on the log worker, including its
final shutdown flush. The host also keeps a bounded tail of callback records.

`utils::output_stderr` is routed to this log path for `RPCS3_UWP`, and console
attachment/stream redirection is disabled. This does not intercept arbitrary
third-party `fprintf(stderr, ...)` calls; those dependencies still need auditing.
Logging is bounded: the callback backlog stops accepting log records at 4096
queued notifications, and the file queue retains the latest 8192 pending lines
under overload. Command completions are not discarded to make room for logs.
Sustained overload can lose log records: `log_stats()` exposes both loss
counters, and the file writer records a warning when file records were dropped.
Log rotation remains work.

## Errors and process ownership

The embedding facade no longer calls `abort()`. C ABI entry points and control
tasks catch C++ exceptions; state-changing failures mark the core faulted.
Fatal reports and stop-watchdog failures become host error notifications.
`thread_ctrl::emergency_exit` reports embedding faults and still finalizes only
an RPCS3-managed worker thread. Unmanaged/control-thread errors unwind to its
guarded task. The embedding build enables MSVC C++ exception unwinding.

The DLL does **not** replace the host's `std::terminate` handler or unhandled
exception filter. UWP guest-memory recovery uses an explicitly enabled SEH
filter around core-owned thread entries and control tasks, not the unavailable
process-global vectored-handler APIs. It is disabled after orderly shutdown.
Unrelated native exceptions continue to the host/OS.

Core thread entries use compiler-generated trampolines with SEH unwind data.
Generated guest-code unwind integration is not implemented, so this UWP profile
normalizes PPU/SPU settings to interpreters after global/per-game configuration
loading. Recompilers are not advertised as supported. This restricts performance
and still requires functional guest-memory recovery tests in AppContainer.

This is not crash isolation: corrupted memory, exceptions through a `noexcept`
frame, raw native faults and termination inside third-party libraries cannot be
safely converted into a normal return by this facade. Fatal state refuses release
instead of claiming that unloading a potentially live core is safe. Remaining
thread/JIT and third-party failure paths require audit and runtime fault tests.

## Brokered storage (API v2 extension)

After the initialize-completed event, while the core is fully stopped and idle,
call `rpcs3_core_mount_storage` with a borrowed `winrt::get_abi(folder/file/stream)`
or file HANDLE. The DLL retains agile WinRT references or duplicates the handle;
it never takes ownership of the caller's reference/handle. Use a unique ASCII
mount name (letters, digits, hyphen; at most 64 characters). A 256-byte output
buffer suffices; `required` includes the terminating NUL. BUFFER_TOO_SMALL does
not register a mount. Serialize mounting and control commands in the host.

StorageFolder mounts expose relative children beneath the returned synthetic
RPCS3 filesystem root. StorageFile, IRandomAccessStream and HANDLE mounts expose
one file named `content`. Join relative game paths to that root, never to
`StorageFile.Path`. Single-file mounts currently lose the filename extension;
extension-sensitive boot detection still needs adaptation.

The host must obtain objects through Windows.Storage/pickers and persist its own
FutureAccessList tokens, then reopen/remount them after restart. Tokens are not
DLL permissions or transferable cross-package grants. The host picker now
persists `rpcs3-game-library`, reopens its StorageFolder asynchronously and
mounts it read-only. The selected folder, or its immediate child game folders,
are recognized by PS3_GAME/USRDIR/EBOOT.BIN, USRDIR/EBOOT.BIN or EBOOT.BIN.
Catalogue launch paths use the synthetic mount, not StorageFolder.Path. The
library is restored after initialization; changes require a stopped, idle core.
Scanning ISO/PKG files, nested libraries and PARAM.SFO metadata remains work.
LocalFolder remains the
native config/cache/log root supplied by the host.

The `fs::device_base` implementation handles stat, enumeration, read/read_at,
seek, size, flush, create, write, truncate, file delete and in-root file moves.
Writes require explicit `writable=1` and the object's existing permission.
Traversal outside the root, alternate streams and drive-qualified names are
rejected. Brokered IO waits on the core worker/MTA, not the UI thread.
HRESULT failures are logged with their numeric code. Native mapping is not
available for WinRT streams. Atomic append, locking, nonrecursive directory
deletion, cross-parent folder moves, timestamp changes and symlinks are unsupported
rather than approximated with unsafe operations. Shutdown clears registered mounts.

## UWP dependency policy

The core profile excludes HIDAPI, libusb, cubeb, desktop XAudio2, OpenAL,
miniupnpc and RtMidi. SDL3-uwp's Windows.Gaming.Input profile replaces desktop
controller handlers; unsupported configured handlers return a Null handler and
an error, not a connected device. USB peripheral emulation is also disabled:
guest USB syscalls return CELL_ENOSYS. UPnP reports inactive; automatic port
mapping and MIDI are unavailable. No Windows.Devices.Midi/USB/UPnP replacement
has been integrated. Audio remains NullAudio.

wolfSSL uses its documented custom-seed hook with BCryptGenRandom as the OS
entropy source, retaining the existing DRBG and security configuration. Seed
errors do not fall back to weak data. A native PC unit test passed seed bounds,
invalid arguments and DRBG initialization/generation after wolfCrypt_Init.
This is not an AppContainer entropy or live TLS handshake test. Guest MAC
addresses derive from the configured PSID, not desktop host-adapter enumeration.

Desktop/system FFmpeg fallback is forbidden for WindowsStore. Configuration
stops unless `RPCS3_UWP_FFMPEG_ROOT` provides all six matching UWP-built libraries
under `lib/`. Supplying a directory is not compatibility certification: those
libraries and all remaining dependencies (including curl/wolfSSL), core Win32
calls and final PE imports still require AppContainer audit and runtime testing.
`RPCS3-UWP/Build-Ffmpeg.ps1` builds pinned FFmpeg n8.0 with MSVC, UWP/WinRT
enabled and desktop devices, native file protocols, network, autodetection and
hardware accelerators disabled. MSVC COFF `.a` archives are accepted alongside
`.lib`; the installed headers must match these libraries. Media decode and
recording use custom AVIO backed by RPCS3 fs::file, including brokered mounts.
The selected PS3 codecs and recording formats are not a complete FFmpeg build.
The initial archives requested LIBCMT and were rebuilt with -MD. Prelink review
of the new archives flags POSIX CRT aliases; final DLL resolution passed the
static UWP import audit. Neither result proves decoder behavior or AppContainer
execution. Use -Clean when changing the FFmpeg CRT profile.

## Verification details

Run `RPCS3-UWP/Test-Embedding.ps1` with the installed MSVC tools. It compiles the
header as C and exercises the independent runtime with a fake backend: ordered
commands, a blocked worker with responsive UI pumping, callback affinity,
recursive-pump rejection, exception containment, error retrieval, persisted
file logs and nonblocking release readiness. These tests do not boot RPCS3.

Add `-CompileIntegration` to compile the actual core facade, App.cpp and
MinimalHost.cpp using existing generated projects and UWP API restrictions.
This requires prior core/host CMake configuration and remains compile-only.

Use `RPCS3-UWP/Audit-Uwp.ps1` after the DLL links to inspect its AppContainer
PE characteristic, required C exports and imported APIs against the installed
SDK whitelist. Unknown package/CRT imports require dependency review; do not
treat an incomplete or prelink report as certification. `-CompileLegacyD3D12`
compiles every imported backend source and reports failures independently.

Pass -PackageManifest with the generated host AppxManifest.xml to validate the
Microsoft UWP VCLibs dependency. APP CRT symbols are checked against exports
of the installed SDK's x64 VCLibs package, not filename suffixes. OS ordinal
imports are resolved locally and the resulting name/module pair is checked
against the SDK whitelist.

On 2026-09-30 the runtime tests passed. `core_api.cpp` and the host's connected
`App.cpp` compiled independently with `WINAPI_FAMILY_APP`. The Release x64 DLL
linked (20,910,080 bytes): all 20 C ABI exports and 544 imports passed static
inspection, including 182 verified APP CRT framework imports, with a matching
host VCLibs dependency. No installed-package execution, PS3 game test or Xbox
validation was performed. The DLL currently uses Null RSX/NullAudio/interpreters.
