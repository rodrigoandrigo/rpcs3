# Configuration audit

Audit date: October 3, 2026. Target: Release x64 UWP, package version 1.0.0.32.

## Coverage

The embedding API enumerates the configuration tree from `cfg_root` rather than maintaining a second list of settings. All 206 emulator settings registered in `rpcs3/rpcs3qt/emu_settings_type.cpp` are represented. The DLL exposes 274 entries including additional advanced settings, VFS mount information, and IPC configuration. Coverage means the setting is visible; it does not establish that every emulator feature or graphics option works in a guest game.

The frontend shows groups, complete names, current values, defaults, numeric limits, enum choices, and UWP restrictions. Names containing slashes are treated as complete registered paths. Structured values such as library overrides, log levels, and anaglyph matrices use validated YAML. Text buffers grow instead of truncating values at 511 bytes.

Graphics opens the real Video settings. Interface settings are separate from the emulated System settings. Interface themes and launch-animation preferences are persisted in LocalState. The misleading storage selector that only changed a displayed path is hidden; it does not relocate emulator data.

The desktop shell processes pending launches when launch animations are enabled. Its Refresh button and menu rescan the brokered game folder rather than only copying the cached catalogue.

ScreenScraper requests use PlayStation 3 system ID 59, as identified by the [official platform page](https://screenscraper.fr/systemeinfos.php?plateforme=59), rather than the sample's system ID 1.

## UWP restrictions

- The host selects the experimental shared D3D12 renderer. The upstream renderer enum remains Null internally because the restored D3D12 backend is instantiated by the embedding adapter.
- Audio output currently uses Null. Keyboard, mouse, camera, music, and microphone handlers are limited to the integrated Null implementations.
- This build lacks LLVM and uses CPU interpreters. Its decoder selections are managed by the host.
- Vulkan and UPnP are excluded. Their settings are shown as unavailable.
- VFS mount paths require brokered storage grants; displaying the existing paths does not allow arbitrary native-path access.
- Desktop IPC has no host integration and its options are read-only.
- DSU controls remain available. Streaming is disabled because this host has no encoder adapter.

These restrictions are enforced by the API as well as the frontend. Reset reapplies the host backend policy. A rejected value preserves the previous setting, and a failed save rolls back the mutation and returns an I/O error. The frontend only reports a completed save after the asynchronous command succeeds.

## Validation

The native regression harness in `rpcs3/Embedded/tests/core_config_test.cpp` loads the actual DLL with the installed x64 UWP VC runtime and uses a new isolated state directory. It exercises enumeration, slash-containing paths, numeric validation, library collections, log maps, invalid input, long text, host restrictions, reset, persistence, failure rollback, shutdown, and release. It does not access the installed application's settings or firmware.

Temporary investigation instrumentation was removed in version 1.0.0.48.
Normal RPCS3 debug configuration options and regression checks remain.

Version 1.0.0.37 passed the 29 API checks and the native D3D12 CPU-access
queue test over 100 iterations. The latter verifies pause service with an
active lease, exclusion of drawing until release, notification preservation,
nested grants and error propagation. It does not execute a guest game or
prove the complete RSX pause path inside AppContainer.

The Qt comparison covers the emulator-setting registry. Qt-specific QSettings preferences, auxiliary managers, debugger windows, device discovery, game-specific configuration workflows, and guest execution are separate capabilities. This audit does not claim full functional parity with every Qt window. The Users/Saves/Trophies/Patches/Cheats tools currently open data directories rather than reproduce the Qt managers.

Build the delivery artifact through `RPCS3-UWP.slnx`; the solution builds the DLL and frontend and signs and verifies the complete MSIXBundle.
