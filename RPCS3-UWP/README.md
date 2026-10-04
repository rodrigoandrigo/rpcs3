# RPCS3-UWP

RPCS3-UWP is an experimental UWP host for the RPCS3 PlayStation 3 emulator. It embeds a Qt-free RPCS3 core DLL, presents the emulator through an ImGui frontend, and offers the restored Direct3D 12 renderer and OpenGL through Mesa Gallium D3D12 in the UWP application's swap chain.

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
- Installed x64 `Microsoft.VCLibs.140.00` UWP framework version 14.0.33519.0
  or newer (used by Meson's native compiler checks).
- Python with Meson, Mako, PyYAML and a Ninja executable; MSYS2 Bison and Flex.
  Set `RPCS3_MESON_PYTHON` to the Python executable containing these packages if
  it is not the default `python` on PATH.

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

1. Builds the bundled Mesa UWP OpenGL libraries with Gallium D3D12 only, then configures and builds the Qt-free `rpcs3-core.dll`.
2. Enables the experimental Direct3D 12 renderer.
3. Uses the external SDL3-UWP source tree.
4. Builds the ImGui frontend and UWP host.
5. Creates the MSIX package.
6. Signs the package with `RPCS3-UWP_TemporaryKey.pfx`.
7. Verifies the resulting signature.

## OpenGL through Mesa

The renderer selector lists `Direct3D 12` first (the default) and
`OpenGL (Mesa Gallium D3D12)` second. Select it under `Video/Renderer` while
emulation is stopped. The stored RPCS3 configuration uses `OpenGL`; the display
name identifies the packaged driver rather than the Windows desktop driver.

The path is RPCS3's existing OpenGL RSX renderer → packaged Mesa `opengl32.dll`
→ `gallium_wgl.dll` / Gallium D3D12 → the host's D3D12 presentation bridge.
The Windows SDK redistributable `dxil.dll` is also packaged to validate and sign
Mesa-generated DXIL shaders; `dxcompiler.dll` is not required or packaged.
Neither the system desktop OpenGL implementation nor Qt is used. Mesa is built
from `../3rdparty/mesa`; no Vulkan or software Gallium fallback is included.

Build the layers individually with:

```powershell
.\RPCS3-UWP\Build-Mesa.ps1 -Python python  # Or the configured Meson Python executable
.\RPCS3-UWP\Build-Core.ps1 -ExperimentalD3D12 -MesaOpenGL
.\RPCS3-UWP\Build-Host.ps1 -MesaOpenGL
```

The initial presentation bridge reads the offscreen OpenGL back buffer on the
CPU, flips its rows, uploads an immutable D3D12 texture and publishes it to the
existing frontend. It uses a 1280×720 drawable; the frontend scales the result
to the application area. This is not zero-copy and adds readback/upload cost.
An OpenGL 4.3 core context and RPCS3's required extensions must be supported by
the selected D3D12 adapter. Native build/probe success does not establish
Xbox AppContainer compatibility or guest-game correctness.

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
    -PackageDlls .\build-uwp-msvc\core\bin\opengl32.dll `
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
- Version 1.0.0.56: 34 configuration API checks (274 entries), including Mesa
  selection/persistence and default ordering; a native OpenGL context and GPU
  clear/readback test using Mesa 26.2.2 / Gallium D3D12 on an AMD Radeon RX 6600M.
  Mesa links the UWP Store C++ runtime, not the desktop VC runtime.

These checks are separate from installed-package, retail Xbox, real firmware, real PKG, game compatibility, performance, and Microsoft WACK testing. Those runtime tiers require appropriate hardware and user-supplied content.

## Troubleshooting

### Title audio preview fails with `Verification failed (object: 0x0)`

Version 1.0.0.58 uses XAudio2 for emulated game audio on the system default
output device, without desktop endpoint enumeration or a Null-backend fallback.
Title audio (`SND0.AT3`) is decoded asynchronously by the bundled FFmpeg decoder
to 48 kHz stereo float PCM and played through XAudio2. Brokered filesystem and
ISO sources use the existing virtual filesystem. Title previews loop until
deactivated and follow the configured volume. Unsupported video previews retain
their thumbnail. Decoder/output failures remain errors, not successful playback.
Native build/configuration checks do not establish audible Xbox/game playback;
that still requires validation on the target device with real content.

### Start remains in the library and reports a remote launch pending

Version 1.0.0.32 distinguishes an already running game from a pending launch. It shows a video status window with Stop when the core is active but has not published a frame. The core no longer silently selects Null when the D3D12 host device is missing, and logs successful shared D3D12 renderer initialization. No-frame status does not prove guest execution is progressing.

### UWP synchronization and surface fixes

The UWP path services external RSX pause requests during startup and while CPU
access leases are active, without discarding leases or bypassing guest locks.
The validated framebuffer layout is committed before guest-memory watches are
registered. Shutdown waits for full stop, including after a previous fault,
with a 30-second deadline and the fault remaining latched.

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

Version 1.0.0.55 replaces UWP preallocated sparse-file guest blocks with
pagefile-backed SEC_RESERVE shared sections. Guest allocations commit only
their used ranges through the writable alias, including stack guard markers.
Both aliases remain shared and normal guest-page protections still apply.
Committed section pages remain committed until the block is destroyed; this
does not remove the Xbox memory budget or prove game execution on Xbox.
The file-backed TAR reader also recognizes zero-filled end-of-archive blocks
instead of reporting valid firmware archive padding as malformed headers.

Version 1.0.0.54 prevents Xbox pointer clicks from also activating the previously
gamepad-focused widget. Tools disables Pause unless the core is running and
Resume unless it is paused. Shared widgets respect disabled state and wait for
the opening confirmation to be released before activating an overlay action.
Command 3 is Pause, not PUP/PKG installation (13/12).
Installed Xbox firmware/package workflows require separate runtime validation.

Version 1.0.0.53 enables gamepad access to the File--Help menu bar. Tap Menu
to toggle menu-bar focus, use the D-pad to navigate, A to open/confirm, and B
to cancel. Desktop ImGui navigation consumes its commands without also firing
retained shell actions such as launching a game or opening settings. Overlay
pages retain their own input handling. Xbox runtime validation remains separate.

Version 1.0.0.52 maps the right stick to ImGui manual scrolling with a 0.35
deadzone, instead of the left stick. Right-stick movement does not activate
buttons; A remains the confirmation button. The artificial R3 "Pointer Press"
alias is removed, while the actual RightThumb input remains available for game
controller mapping. Xbox controller runtime validation remains separate.

Version 1.0.0.51 uses the full CoreWindow area instead of the Xbox safe-area
inset. Font sizes use the same pixel units as the controls, avoiding oversized
labels in fixed-size buttons. The composition swap chain follows the panel's
actual dimensions and per-axis composition scale, including subsequent changes.
Build and package validation do not replace Xbox display validation.

Version 1.0.0.50 disables the Xbox automatic UWP layout scaling before creating
the XAML interface. Desktop DPI handling and window resizing are unchanged.
Xbox runtime validation remains a separate step.

Version 1.0.0.49 hides the library interface while a game is running and stretches
the video to fill the app client area, without a floating video window or borders.
Before the first frame, the game area is black. Press Escape or hold Menu+View
on a gamepad to stop and return to the library. This does not change the OS
window mode or remove the system title bar.

Version 1.0.0.48 removes the temporary investigation instrumentation, shader
failure dumps and pixel readbacks. Normal RPCS3 logging and all functional
corrections remain. The application no longer creates the temporary frontend
video trace. Previously created user logs are not deleted.

Version 1.0.0.47 presents the core video plane as opaque. The frontend's core
frame SRV preserves RGB and maps alpha to one, so valid RGB frames with zero
display-buffer alpha do not disappear under ImGui blending. Internal RSX pixels,
rendering/blending, and ordinary UI texture mappings are unchanged. The native
`d3d12_video_alpha_test.cpp` checks the SDK component selectors and zero/partial/
full-alpha inputs; installed GPU presentation remains a separate validation step.

Version 1.0.0.46 retains each vertex shader's constant relocation table and uses
it when uploading the D3D12 constant buffer. Indexed-constant shaders continue
to upload the full RSX bank; the branch-bit slot remains unchanged. Installed
game rendering remains a separate validation step.


Version 1.0.0.42 defines the shared RSX `_saturate` helper in D3D12 HLSL using
`clamp(x, 0, 1)`, matching the shared GLSL expression. The native shader regression
accepts a captured fragment shader with the optional `ps_5_0` argument, reproduces
the missing-helper X3004, and compiles the corrected full shader. Installed-game
rendering and edge-case floating-point conformance remain unverified.

Version 1.0.0.41 adapts scalar literal vector constructors emitted by the shared
decompilers to explicit HLSL components in the D3D12 vertex and fragment paths.
It also defines vertex `_fetch_constant` against the existing per-draw `vc`
constant buffer and maps the shared float `fma` operation to HLSL `mad` (the
Shader Model 5 `fma` intrinsic only accepts doubles). This uses SM5 float math;
exact RSX rounding equivalence is not established. Other renderers and
expression/vector constructors are unchanged.
The native `d3d12_shader_splat_test.cpp` reproduces X3014 in the captured shader
and compiles the corrected full shader with D3DCompile. This is shader compiler
validation, not an installed-game rendering test.


RPCS3-UWP contains code derived from RPCS3 and uses third-party components with their respective licenses. See the repository license files and `Frontend/THIRD_PARTY_NOTICES.md` before redistribution.
