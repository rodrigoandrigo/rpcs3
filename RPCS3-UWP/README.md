# RPCS3-UWP

RPCS3-UWP is an experimental UWP host for the RPCS3 PlayStation 3 emulator. It embeds a Qt-free RPCS3 core DLL, presents the emulator through an ImGui frontend, and displays the legacy Direct3D 12 renderer through the UWP application's swap chain.

The project is intended for Windows UWP/AppContainer environments and Xbox development scenarios. It is not an upstream RPCS3 build and does not use the desktop Qt frontend.

## Project layout

- `RPCS3-UWP.slnx` is the main Visual Studio solution.
- `RPCS3-UWP.vcxproj` is the orchestration project used by the solution.
- `Build-Solution.ps1` builds the core, frontend, package, and signature.
- `FrontendHost` contains the RPCS3-specific UWP application and host integration.
- `Frontend/UWP-ImGuiFrontend` contains the reusable ImGui frontend components.
- `../rpcs3/Embedded` contains the public C embedding API and the Qt-free RPCS3 DLL integration.
- `../rpcs3/Emu/RSX/D3D12` contains the restored experimental Direct3D 12 RSX renderer.

Generated files are written to `../build-uwp-msvc`. They are not source files and should not be added to `RPCS3-UWP`.

## Architecture

The application is split into three layers:

1. `rpcs3-core.dll` owns the emulator state and runs control operations on a dedicated worker thread.
2. `RPCS3FrontendRuntime.dll` connects the public C API to the UWP host, brokered storage, callbacks, settings, and the ImGui frontend.
3. `RPCS3-UWP.exe` owns the UWP window, input, D3D12 device, command queue, swap chain, and package lifecycle.

The core never owns the application process. It does not call `exit()` to terminate the host, and errors are returned through API results and callbacks. Logs are delivered to the frontend and written below `ApplicationData.Current.LocalFolder`.

The frontend provides:

- RPCS3-style desktop layout with menus, toolbar, search, list/grid views, and Log/TTY panels.
- Dynamic access to the complete Qt-independent RPCS3 configuration tree.
- Numeric limits, defaults, validated structured settings, and explicit UWP backend restrictions. See [Configuration audit](CONFIGURATION-AUDIT.md) for coverage and remaining differences from Qt.
- UWP pickers and Future Access List integration for game folders, firmware, and packages.
- Firmware PUP and PKG installation through broker-mounted files.
- User, save-data, trophy, savestate, screenshot, patch, cheat, VFS, and log data access.
- Controller mapping and RPCS3 execution controls.
- Shared D3D12 frame presentation in the UWP swap chain.

Qt and desktop-only backends are not linked into the package. Desktop debugger windows and device backends that are not valid in an AppContainer are replaced by the exported core state, event, configuration, and logging APIs.

## Requirements

- Windows 10 or Windows 11 x64.
- Visual Studio 2026 with the Desktop C++ and UWP C++ workloads.
- Windows SDK `10.0.26100.0`.
- MSYS2 installed at `C:\msys64` with the UCRT64 environment.
- CMake available at `C:\msys64\ucrt64\bin\cmake.exe`.
- Git for restoring pinned third-party sources when required.
- SDL3-UWP source tree at `C:\Users\rodri\Dev1\Projetos\SDL3-uwp`.
- `RPCS3-UWP_TemporaryKey.pfx` for local test-package signing.

The scripts enter the Visual Studio MSVC environment automatically. MSYS2/UCRT64 supplies the build shell and CMake, while UWP C++ code is compiled with MSVC.

## Restore dependencies

If the RPCS3 source snapshot does not contain all required submodules, restore the pinned dependencies before configuring the project:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\RPCS3-UWP\Restore-Dependencies.ps1
```

Check the local toolchain and required source trees with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\RPCS3-UWP\Check-PortPrerequisites.ps1
```

FFmpeg is optional in the current UWP configuration. To build the restricted static UWP-compatible FFmpeg subset:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\RPCS3-UWP\Build-Ffmpeg.ps1
```

## Build with Visual Studio

Open `RPCS3-UWP\RPCS3-UWP.slnx` in Visual Studio, select `Release` and `x64`, and build the solution.

The solution performs the following operations automatically:

1. Configures and builds the Qt-free `rpcs3-core.dll`.
2. Enables the experimental Direct3D 12 renderer.
3. Uses the external SDL3-UWP source tree.
4. Builds the ImGui frontend and UWP host.
5. Creates the MSIX package.
6. Signs the package with `RPCS3-UWP_TemporaryKey.pfx`.
7. Verifies the resulting signature.

## Build from PowerShell

Run MSBuild against the same solution used by Visual Studio:

```powershell
. .\RPCS3-UWP\Enter-MsvcEnvironment.ps1
msbuild .\RPCS3-UWP\RPCS3-UWP.slnx `
    /t:Build `
    /p:Configuration=Release `
    /p:Platform=x64 `
    /m:1 `
    /nr:false
```

Rebuild everything:

```powershell
msbuild .\RPCS3-UWP\RPCS3-UWP.slnx `
    /t:Rebuild `
    /p:Configuration=Release `
    /p:Platform=x64 `
    /m:1 `
    /nr:false
```

Clean generated targets:

```powershell
msbuild .\RPCS3-UWP\RPCS3-UWP.slnx `
    /t:Clean `
    /p:Configuration=Release `
    /p:Platform=x64
```

`Build-Solution.ps1` may also be called directly:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\RPCS3-UWP\Build-Solution.ps1 `
    -Action Build `
    -Configuration Release `
    -Platform x64
```

## Build outputs

Core DLL:

```text
build-uwp-msvc\core\bin\rpcs3-core.dll
```

UWP executable and runtime DLL:

```text
build-uwp-msvc\host\Frontend\RPCS3-UWP-Host\Release\RPCS3-UWP.exe
build-uwp-msvc\host\Frontend\RPCS3-UWP-Host\Release\RPCS3FrontendRuntime.dll
```

Test-signed MSIX package:

```text
build-uwp-msvc\host\Frontend\RPCS3-UWP-Host\AppPackages\RPCS3UwpApp\
```

The exact package directory includes the manifest version, architecture, and `_Test` suffix.

## UWP API audit

After a successful build, audit the DLL imports and package manifest:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File .\RPCS3-UWP\Audit-Uwp.ps1 `
    -Dll .\build-uwp-msvc\core\bin\rpcs3-core.dll `
    -PackageManifest .\RPCS3-UWP\FrontendHost\UWP-App\Package.appxmanifest
```

`STATIC_IMPORTS_PASS` means that the static import audit found no unsupported API. It does not prove successful package activation, firmware installation, game execution, Xbox compatibility, or WACK certification.

## Brokered storage

UWP does not grant arbitrary filesystem access. The frontend therefore uses `FileOpenPicker`, `FolderPicker`, and the Future Access List. Selected `StorageFile` and `StorageFolder` objects are mounted through the public embedding API and exposed to RPCS3 through synthetic VFS paths.

Do not reconstruct native paths from picker results. The brokered object or its Future Access List token is the access grant.

## Packaging and signing

The normal solution build signs the newest generated MSIX automatically. The included PFX is a development certificate and is not suitable for public distribution.

For Store or production distribution, replace the development identity and certificate with the identity assigned to the published application. Never commit private production signing keys.

## Current validation level

The current source has been validated through:

- Release x64 compilation with MSVC.
- UWP host and core linking.
- MSIX creation and development signing.
- Static AppContainer import auditing.
- Public C ABI compilation.
- Brokered-path confinement tests.
- D3D12 copy, readback, surface-coherency, MSAA, and shader-path tests.

These checks are separate from installed-package, retail Xbox, real firmware, real PKG, game compatibility, performance, and Microsoft WACK testing. Those runtime tiers require appropriate hardware and user-supplied content.

## Troubleshooting

### Start remains in the library and reports a remote launch pending

Version 1.0.0.32 distinguishes an already running game from a pending launch. It shows a video status window with Stop when the core is active but has not published a frame. The core no longer silently selects Null when the D3D12 host device is missing, and logs successful shared D3D12 renderer initialization. No-frame status does not prove guest execution is progressing.

### Boot fails with Windows error `0x5AF` (commit/pagefile limit)

Version 1.0.0.31 removes the unused 32 GiB hook reservation and copy-on-write mapping from the UWP interpreter path. The retained guest RAM mappings are separate and remain necessary. C++ command failures are reported through the embedding error callback and command completion result instead of being rethrown from the command wrapper. The core is marked faulted, so the host must stop it before retrying.

Earlier versions may leave `rpcs3/tmp/rpcs3_vm_sparse.tmp` behind after a crash. This version does not create or map that file. Existing files are not automatically deleted; never remove emulator temporary files while any emulator instance is running.

### A square flashes on Start, then the application crashes in generated SPU code

Version 1.0.0.30 skips launch animations when artwork is missing. The UWP static SPU path calls the existing C++ interpreter directly instead of entering a generated gateway. Interpreter restarts unwind through a scoped C++ exception rather than a generated escape trampoline. Guest-game validation remains separate from build and configuration tests.

### The game starts and the application closes without a fatal log message

Windows Application events can contain the native fault even when RPCS3's log has no fatal entry. Version 1.0.0.29 fixes first-use clock initialization in `sys_time_get_current_time`, which could subtract a newer epoch from an older counter and trigger integer overflow (`0xC0000095`). The native `uwp_clock_epoch_test.cpp` regression reproduces the overflow precondition and checks the corrected sampling order. Installed-game validation remains separate.

The `rpcs3/tmp/rpcs3_vm_sparse.tmp` file has a logical size of 32 GiB and backs the sparse hook mapping. Logical length is not physical disk consumption; inspect its allocated ranges before judging storage use. It is not a game download and must not be deleted while the core is running.

### Refresh rescans the folder but the game list does not change

Version 1.0.0.28 synchronizes the frontend catalogue after each completed brokered folder scan. Refresh requires a stopped, initialized core; attempts during emulation show a notification. The Null audio backend now keeps buffering disabled, optional AppContainer page pinning is not reported as a desktop limit error, and unavailable system-wide CPU measurements are explicitly labeled unavailable.

### Boot fails in `vm::ps3_::init` with Windows error `0x57`

Version 1.0.0.27 maps the sparse copy-on-write hook section with `MapViewOfFileFromApp`, allowing Windows to select its address. The previous fixed-address mapping failed on the tested host. The native regression test `rpcs3/Embedded/tests/uwp_hook_mapping_test.cpp` checks the 32 GiB view, boundary writes, and zeroed contents after remapping. This test does not establish installed AppContainer or game compatibility.

### Visual Studio tools were not found

Install the required C++ workloads and verify that `vswhere.exe` is present below the Visual Studio Installer directory.

### SDL3-UWP was not found

Place the SDL3-UWP source at `C:\Users\rodri\Dev1\Projetos\SDL3-uwp`, or update `Build-Core.ps1` and `Check-PortPrerequisites.ps1` to use the intended location.

### The package is unsigned

Verify that `RPCS3-UWP\RPCS3-UWP_TemporaryKey.pfx` exists and that the certificate publisher matches the package manifest identity.

### A picker works but RPCS3 cannot open the item

Confirm that the selected item was stored in the Future Access List and mounted through `rpcs3_core_mount_storage`. A visible filesystem path alone does not grant AppContainer access.

### The build reports missing OpenCV

OpenCV is optional for this UWP build. The warning is expected when the desktop OpenCV dependency is unavailable.

## License

RPCS3-UWP contains code derived from RPCS3 and uses third-party components with their respective licenses. See the repository license files and `Frontend/THIRD_PARTY_NOTICES.md` before redistribution.
