# UWP ImGui Frontend

`UwpImGuiFrontend` is a reusable C++20 component for controller-first native
UWP applications that already own an ImGui graphics backend. The folder has no
source-code dependency on a particular application, emulator core, renderer,
or global configuration system. Applications provide those behaviors through
the host and service interfaces.

## Portable Kit

This repository is a portable kit that can be moved into another native UWP
repository without carrying a host application's source tree. Its top-level
`CMakeLists.txt` builds the reusable library and a separate, runnable UWP sample:

- `UWP-ImGuiFrontend` contains the public headers, implementation, assets, and
  host-logic tests
- `../FrontendHost` contains the RPCS3 UWP XAML host,
  its `IFrontendHost` adapter, D3D12 renderer, WGI input, and app manifest
- `dependencies/imgui` contains only the ImGui core and D3D12 backend files the
  sample compiles, together with ImGui's license
- `cmake/triplets/x64-uwp-static.cmake` and `vcpkg.json` describe the native x64
  UWP dependency build

The kit does not vendor vcpkg, Visual Studio, or the Windows SDK. `Build.ps1`
accepts a vcpkg checkout and produces the sample MSIX.
`tools/VerifyCopies.ps1` validates the root and component vcpkg manifests plus
the files required by the standalone build.

## Included UI

- Dynamic home shell based on a 1280x720 reference layout, expanding to fill
  wider and taller UWP/Xbox viewports
- Retained catalogue grid, side rails, D-pad and analog navigation, pointer
  hit testing, page navigation, context actions, and companion-driven selection
- Themes, rounded panels, cursor bloom, title-pill animation, page indicators,
  optional action hints, controller/battery HUD, lazy artwork, fan-art, and
  video-frame hooks
- Configurable launch handling with the shell's zoom, hold, artwork fade, and
  black transition before the host launch callback
- Themed buttons, selectables, toggles, combo popups, float/integer sliders,
  steppers, color editors, progress bars, section headings, and scroll panes
- Split navigation/content views, virtualized artwork data tables, action lists,
  choice dialogs, quick menus, form editors, text-editor panels, confirmations,
  progress dialogs, modal helpers, and timed notification cards
- A complete reusable split-page settings view with System, DSU/Streaming,
  Storage, ScreenScraper, configurable Community links, and host extension tabs
- A host-populated Graphics split view with no predefined emulator settings;
  applications supply every category, control, value, and callback
- A controller-first Library/Title Manager table with a headerless list,
  retained toolbar-to-row navigation, aspect-preserving artwork at a 58-unit
  height, 120% focused scaling, launch/context actions, refresh, and add-content
  hooks
- Native UWP `CoreTextEditContext` text editing for ImGui fields, including the
  Xbox on-screen keyboard, caret/selection updates, delete, submit, and focus
  restoration
- UWP file and folder pickers with optional `FutureAccessList` persistence
- A reusable storage-location dropdown for Development files, USB storage, and
  custom-folder targets, with custom access retained through the folder picker
- A small generic icon pack under `UWP-ImGuiFrontend/assets/icons`, controller
  preview images under `UWP-ImGuiFrontend/assets/controller-previews`, the Lato
  Medium interface font under `UWP-ImGuiFrontend/assets/fonts`, its OFL license,
  and a CMake helper for copying these assets into an application package

## Included Services

- ScreenScraper client with resumable scans, metadata and media downloads,
  quota handling, retries, manifests, and per-platform fallback media
- ES-DE media-tree interoperability through `MakeEsDeMediaLayout`, including
  platform/type folders, ROM-relative filenames, and reuse of existing media
- UWP `PasswordVault` storage for end-user ScreenScraper credentials
- Generic ScreenScraper settings panel using the same controller-friendly
  widgets as the shell
- Controller-first metadata editor and full-screen ScreenScraper match browser,
  including debounced asynchronous search, stale-result rejection, artwork
  previews and cycling, five-star ratings, native text input, and unsaved match
  staging until the host confirms an atomic save
- Match-browser LB/RB cycling across only the selected result's available media,
  half-star ratings with white filled and grey unfilled portions, dynamically
  sized descriptions, trigger-input isolation, and OSK-to-result focus recovery
- DSU companion extensions for catalogue transfer, remote selection and launch,
  keyboard navigation, stream authorization, and injected transport/encoder
  adapters
- Generic DSU and streaming settings panel; the first enabled endpoint carries
  companion extensions automatically, so hosts do not expose a duplicate
  transport selector
- A host-neutral controller profile model and controller-first visual mapper
  with Player 1-8 tabs, persistent controller/device/profile selectors, bindings
  grouped around a controller preview, stick/trigger tuning, motion/pointer
  mappings, input sources, full/merged DSU routing, profiles, live input,
  capture, device-capability-aware dead-zone/range/rumble controls, and advanced
  expressions. Hosts can persist DSU routes globally or inside per-title profiles
- Reusable atomic controller-profile persistence for active, per-title, and
  named profiles, plus an applied DSU route table that validates each player and
  gates input by server ID, remote slot, and gamepad/motion/touch feature
- A reusable full-screen title-options split view with actions on the left and
  fixed-aspect artwork, metadata, enabled/disabled reasons, check states, and
  application callbacks on the right

The application remains responsible for catalogue discovery, launch behavior,
configuration persistence, texture upload/release, video decoding, stream
encoding, standard DSU controller packets, and the behavior behind app-specific
settings extension pages. These remain host responsibilities behind the public
adapters.

The default rail is also portable: Home is rendered by `Frontend` and
`ShellRenderer`, Library uses `LibraryView`, Settings uses `SettingsView`, and
Settings can draw `CompanionPanel` for DSU/streaming, while Controllers can host
the global `InputMapperView`. Graphics uses `GraphicsView`; the frontend owns
only its controller-first two-pane presentation while the host supplies all
categories, controls, values, and behavior.
Power invokes `HostCommand::ExitApplication`; it is an operation rather than a
page. Applications may replace any rail action in `FrontendConfiguration`.

Catalogue entries use a stable `ItemId` and may carry content IDs, versions,
regions, formats, metadata, launch paths, and typed media assets. The host owns
catalogue discovery and returns snapshots through `ICompanionHost`; the shared
frontend owns selection, paging, presentation, and launch arbitration.

## Filesystem Locations

Use `DrawStorageLocationSelector` for every user-configurable folder path. A
host can use `MakeXboxStorageLocationPresets` for the standard Development files
and USB storage choices; the shared control adds the custom-folder choice and
opens the native UWP folder picker. The helper supplies standard labels, while
the host supplies its own application-specific paths. Custom selections are
retained in `FutureAccessList` with a stable token derived from the selector ID
unless the host supplies its own token or explicitly disables persistence.
`LibraryView` can expose the same selector as an expandable Games directory flow;
use that path for Add/Open Content and Title Manager pages so controller users do
not have to type a filesystem path.

Do not expose filesystem folders through `NativeInputText`. Text entry remains
appropriate for URLs, network hosts, account details, names, and command or
input bindings. File-valued settings should use `IPickerService::PickFile`
instead of the folder-location selector.

For media shared with ES-DE, set each `ScreenScraperTarget::esDeMedia` with
`MakeEsDeMediaLayout`. Keep `mediaDirectory` pointed at an application-owned
per-game sidecar directory; only images and videos are placed under ES-DE's
`downloaded_media/<system>/<media-type>` tree. The resolver mirrors ROM
subfolders and uses the ROM stem, so ES-DE can consume downloads without a
configuration or code change.

The ES-DE mapping uses `covers`, `3dboxes`, `fanart`, `screenshots`, `marquees`,
`backcovers`, `physicalmedia`, `videos`, and the standard `miximages` directory
for Recalbox Mix V2. Recalbox Mix V1 uses `miximages-v1` so both mixes can exist
without overwriting ES-DE's single standard mix-image slot. Existing `.png`,
`.jpg`, and `.webp` images and `.mp4`, `.mkv`, `.avi`, `.mov`, `.wmv`, `.m4v`,
and `.webm` videos are discovered. New ScreenScraper images use `.png` and videos
use `.mp4`; the consuming host's texture and video adapters must decode every
existing format it intends to use.

Set `ShellPresentation::font` to an atlas font containing the glyph ranges the
catalogue requires. The renderer scales that font from layout units, so Unicode
title text does not need a separate renderer implementation. The runnable sample
loads the packaged Lato Medium face with the extended Latin, punctuation,
currency, arrow, mathematical-symbol, and presentation-form ranges used by the
frontend.

## Integration

```cmake
set(UWP_IMGUI_FRONTEND_IMGUI_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/imgui"
    CACHE PATH "Directory containing imgui.h")
add_subdirectory(path/to/UWP-ImGuiFrontend)
target_link_libraries(MyUwpApp PRIVATE
    UwpImGuiFrontend::UwpImGuiFrontend)

uwp_imgui_frontend_copy_assets(MyUwpApp
    "$<TARGET_FILE_DIR:MyUwpApp>/resources")
```

In this example, `IFrontendHost::ResourceRoot()` returns the packaged
`resources` directory; the default shell paths then resolve under
`resources/icons`.

The standalone top-level build already points `UWP_IMGUI_FRONTEND_IMGUI_DIR` at
the bundled `dependencies/imgui`. When embedding only the component, set it
before `add_subdirectory` unless `imgui.h` is in the component's sibling
`imgui` directory. Include the complete public API with:

```cpp
#include <UwpImGuiFrontend/UwpImGuiFrontend.h>
```

The library requires C++20, Boost headers, fmt, RapidJSON, and the Windows UWP
application libraries. It does not create an ImGui context, command queue,
swap chain, texture cache, audio device, or graphics backend.

Implement `IFrontendHost`, create the ScreenScraper and companion services,
then construct `Frontend` and `ShellRenderer` after the ImGui context and
graphics backend exist. A frame that uses the native text service and retained
widgets has this order:

```cpp
InitializeNativeTextInput(); // once, after ImGui::CreateContext()
frontend.Initialize();

SubmitImGuiNavigationInput(frameInput, controllerConnected);
graphics.BeginFrame(frameInput.deltaSeconds); // backend NewFrame + ImGui::NewFrame()
BeginNativeTextInputFrame();
BeginWidgetFrame(theme, currentInput, previousInput, overlayOwnsInput);
if (!overlay.HandleInput(frameInput))
    frontend.HandleInput(frameInput);
shellRenderer.Draw(frontend, presentation, frameInput.deltaSeconds);
overlay.Draw(); // App-specific pages built with Widgets.h and Views.h.
EndWidgetFrame();
EndNativeTextInputFrame();
graphics.EndFrame(); // ImGui::Render() and graphics submission

frontend.Shutdown();
ShutdownNativeTextInput(); // before destroying ImGui or CoreWindow
```

Call `SubmitImGuiNavigationInput` only when the application's ImGui platform
backend does not already submit controller navigation. Overlay input must be
handled before `Frontend::HandleInput`, and an overlay that closes must retain a
release barrier until its closing command is released.

`../FrontendHost/MinimalHost.cpp` is the RPCS3 host adapter. It implements
the shell lifecycle, every widget family, split views, a virtualized table,
ScreenScraper and DSU/streaming panels, the metadata editor and match browser,
native text editing, UWP pickers, the storage-location dropdown, selection and
confirmation dialogs, an overlay text editor, action lists, notifications, the
shared full-screen title-options view, the generic Graphics split page, and the
complete global and per-title controller mapper. Select the Controllers rail
for the global mapper. When a host supplies catalogue entries, focus one and
press `X` for the title-options view, then choose Controller Mapping for its
per-title mapper. The portable title-options default contains only Start,
Favorite, Edit Metadata, and Controller Mapping; an emulator supplies any
additional application-specific actions explicitly.

`../FrontendHost/UWP-App` supplies the executable host: a native
`IFrameworkView`/`CoreWindow`, D3D12 swap chain and ImGui backend, WIC texture
loader, WGI Xbox controller input, keyboard and pointer input, suspend/resume
handling, package assets, and `Package.appxmanifest`. The sample starts with an
empty catalogue and stores active, per-title, and named controller profiles
atomically in LocalState through the reusable controller-profile store. Its input
host applies saved DSU routes and accepts a channel only when the enabled server,
remote slot, and gamepad/motion/touch selection match. Launching a supplied title
activates its per-title routes and stopping it restores the global routes. It
does not synthesize games or represent frontend pages as titles. It ships without
ScreenScraper developer credentials, catalogue discovery, a DSU packet backend,
or a streaming encoder. WGI exists only in this sample adapter; the
reusable frontend consumes backend-neutral device descriptors, capture samples,
and `FrameInput`. Remote ScreenScraper requests, DSU controller traffic,
streaming, launch logic, and application-specific settings therefore remain
behind their documented host adapters.

## Graphics Page

Populate `GraphicsViewModel::pages` with stable IDs, labels, and draw callbacks.
`GraphicsView` supplies the same retained navigation/content split used by the
other frontend pages, including controller focus transfer, right-stick scrolling,
and back handling. It intentionally contains no resolution, frame-rate, graphics
pack, enhancement, reload, download, or preset assumptions. Each consuming
application supplies the controls that make sense for its renderer.

The Graphics rail in the runnable sample opens the empty scaffold and contains
no sample settings.

## Input And Focus

Supply one `FrameInput` every frame. It carries directional input, accept, back,
context, alternate, menu, view, shoulder buttons, both analog sticks, and pointer
state; the application decides which keyboard, controller, touch, or remote
inputs produce those values. `Frontend` handles edge detection and held-direction
repeat for the retained shell. Reusable widgets receive both current and previous
button states through `BeginWidgetFrame`.

The context action opens a title page only while a real catalogue item or Library
table row owns focus. Hosts should leave it inert over rails, empty placeholders,
folder selectors, and other page controls.

Give an open overlay or dialog the first chance to consume input before calling
`Frontend::HandleInput`. This keeps gameplay and the underlying shell from
responding while a quick menu, settings page, picker, or metadata editor owns
focus. Pass `NativeTextInputConfiguration::setInputCapture` when the host also
needs an explicit signal to suppress its normal controls while the UWP text
service or Xbox on-screen keyboard is active.

Hosts whose ImGui platform backend does not already submit controller keys may
call `SubmitImGuiNavigationInput` before `ImGui::NewFrame`. Nested views consume
`B` from the inside out: a popup closes before its page, metadata and mapping
views return to title options, and title options return to the shell. Keep shell
input behind a release barrier when an overlay closes so its final `A` or `B`
press cannot activate the item underneath it. The sample demonstrates this
ownership and focus flow.

Native UWP hosts must also subscribe to
`SystemNavigationManager::BackRequested`, set `Handled(true)`, and route that
one-shot request into `FrameInput::back`. On Xbox this prevents the platform
back action from terminating the application while the frontend is closing its
current popup or page.

## Controller Mapping

Implement `IInputMappingHost` to connect an application's physical devices,
emulated controls, profiles, settings, and persistence to `InputMapperView`.
The frontend stages complete `ControllerProfileDraft` values; Save submits every
modified player through `SaveProfileDrafts` as one batch, and Cancel discards the
drafts. Hosts that persist more than one player should override the default batch
implementation to validate and commit the set atomically.
Open the mapper with no title ID for the application's global Controllers page,
or provide a stable title ID for a per-game profile.

Hosts without an existing controller-profile format can use
`ControllerProfileStoreSnapshot`, `LoadControllerProfileStore`, and
`SaveControllerProfileStoreAtomic`. The store preserves named profile identity,
controller and device settings, mappings, and all DSU route fields.
`DsuRouteTable::Apply` validates and installs routes; a standard DSU packet
backend should call `Accepts` before forwarding each gamepad, motion, or touch
channel. This makes remote-slot and feature selections runtime behavior rather
than display-only settings.

The mapper uses a spatial, player-tabbed layout modelled after full desktop
controller configurators rather than a category list. Its Controller page keeps
all standard bindings visible around a controller preview. Motion & Pointer
always shows accelerometer, gyroscope, pointer, and touch bindings, even when a
compatible device is disconnected. Input Sources contains attached/default
devices, device settings, and an explicit DSU player route. Profiles & Advanced
contains named profiles, defaults, capture behavior, live input, and expression
editing.

`IInputMappingHost::GetControllerPreviewTexture` supplies the image for each
emulated controller type. The host owns the texture and its lifetime. The mapper
uses the full preview height and derives width from the texture's native aspect
ratio; it does not draw a procedural controller or force images to one width.
`MakeXboxOneSeriesControllerDescriptor` provides the shared Xbox One / Series
controller entry used by the sample; hosts include it only when that emulated
controller type is appropriate for their application.
The sample also supplies Wii U GamePad, Wii U Pro Controller, Wii U Classic
Controller, and Wii Remote descriptors. Applications choose which emulated
controller types to expose.

It supports eight logical players, named and per-title profiles, multiple
attached devices with one default device, connect/calibrate/rumble actions,
stick and trigger deadzone/range settings, host-provided controller options, and
DSU Full Controller or Merge routes. DSU is also presented as a standard
mappable physical device, so its buttons, sticks, triggers, motion, and touch
may be captured or used in device-qualified expressions.

Use the D-pad to move spatially without entering or leaving panes. Use `A` to
activate controls and begin mapping capture, `Menu` for the advanced expression
browser, `View` or the visible Reset action for defaults, `LB/RB` to change
players, and `B` to return. During capture every controller button is reserved
for the new binding until capture succeeds or times out, and navigation resumes
only after the captured input is released. The advanced expression browser uses
`Y` to add an OR input. Clearing mappings remains an explicit menu action; no
face button silently deletes a binding. The expression engine is independent of
ImGui and supports device references, chords, arithmetic, comparisons,
conditionals, variables, assignments, shaping functions, and stateful timing
functions. Invalid expressions remain visible and evaluate neutrally.

Capture has a three-second overall timeout, waits 150 ms for neutral input, and
collects chords for 350 ms, or 750 ms when alternate-input waiting is enabled.
Expressions support `if`, `not`, `abs`, trigonometric functions, `sqrt`, `pow`,
`min`, `max`, `clamp`, `deadzone`, `timer`, `toggle`, `smooth`, `hold`, `tap`,
`relative`, and `pulse`. Default limits are 4,096 characters, 512 tokens, 64
nesting levels, and 128 stateful nodes; digital values use a 0.5 threshold and
non-finite results evaluate as neutral input.

## Reusable Settings

Create a `SettingsView`, populate `SettingsViewModel`, and provide the service
and persistence adapters in `SettingsViewServices` and `SettingsViewCallbacks`.
The built-in pages expose every host-neutral frontend option. Add emulator or
application-specific pages with `SettingsExtensionPage`; selecting a category
updates the content pane immediately, and D-pad left/right moves between panes.
The settings view does not move or replace settings already owned by a consuming
application.

The built-in pages contain:

- **System:** theme selection, action hints, and launch animations
- **DSU/Streaming:** per-server enable state, host, port, streaming URL, and
  optional multiple controller endpoints. The first enabled server automatically
  carries companion extensions; player assignment, Full Controller/Merge mode,
  motion, and touch routing are configured in the controller mapper. Streaming
  remains a separate switch because it controls media rather than controller
  packets
- **Storage:** any host-provided collection of Development files, USB storage,
  or custom-folder selectors
- **ScreenScraper:** enable state, secure user credentials, download controls,
  overwrite behavior, preferred region, list and detail artwork, dynamic
  backgrounds, media storage, progress, stop, and resumable scans
- **Community:** configurable grouped links and independent actions

ScreenScraper download controls cover metadata, box art, fan art, videos,
screenshots, and logos. Artwork selectors cover 2D box, 3D box, Recalbox Mix V1,
Recalbox Mix V2, fan art, screenshot, game logo, 2D box back, and 2D cartridge
media. Dynamic backgrounds cover Off, fan art, screenshot, video, Recalbox Mix
V1, and Recalbox Mix V2. Region preference covers Auto, USA, Europe, Japan, and
World.

Artwork resolution remains within the selected media type and its regional
variants. It does not substitute unrelated local or ScreenScraper artwork. Game
logos are the sole type fallback: `wheel-hd` is preferred and may fall back to
`wheel`.

The Community page defaults to a Discord selector for Xbox Emulation
Hub and Revives Community Server and the separate APP Store action. Hosts may
append, replace, or clear these model entries without changing the view.

## Metadata And Matching

`MetadataEditor` edits title, description, half-star rating, release date,
developer, publisher, genre, and player count. It supports D-pad navigation,
accept/back actions, right-stick scrolling, native text entry, current-artwork
previews, and atomic host-controlled saving. Stored dates are normalized and
displayed as `DD/MM/YYYY`. ScreenScraper text is normalized to UTF-8 and common
HTML/XML character references are decoded before display or persistence; the
host-provided font must still contain the required glyphs.

The full-screen match browser begins an asynchronous search after two non-space
characters with a 200 ms debounce. New searches invalidate stale responses.
Results include artwork, title, platform, release date, rating, description,
publisher, developer, genre, and players. LB/RB cycles only through media types
available for the selected result. Choosing a result stages its metadata and
enabled media in the editor; nothing is persisted until the user chooses Save.

## Credentials And Ownership

ScreenScraper developer credentials are supplied in each
`ScreenScraperRequest`; they are never embedded in this library. End-user
credentials can be stored with `CreatePasswordVaultCredentialStore`. A resume
request must include the application's current catalogue defaults so older
progress files can be upgraded with system IDs and media directories.

The ScreenScraper and companion implementations each support one live service
instance per process. Their factories provide ownership and lifecycle. Shut
them down before destroying their host, transport, or graphics dependencies.

ScreenScraper requests run asynchronously. Do not destroy the service, its
credential store, picker, or host callbacks while a request is active. Call
`Frontend::Shutdown` before releasing these dependencies; it stops the companion
service and requests cancellation of the active ScreenScraper operation.

The companion service carries extensions alongside an application's DSU
transport. The application retains ownership of the UDP socket, standard DSU
controller traffic, configuration persistence, and media encoder. The shared
extension supports catalogue and platform-media transfer, remote selection and
launch, navigation state, running-content announcements, stream authorization,
and main or auxiliary screen requests. Hosts with independent encoders may expose
the main screen plus up to four auxiliary screens; legacy single-encoder hosts
can retain the main/secondary adapter methods.

## Verification

The portable kit requires Visual Studio 2022 with C++ UWP support, CMake 3.21 or
newer, a Windows 10 SDK at least as new as 10.0.19041.0, and a vcpkg checkout.
From the repository root, configure, build, and package the sample with:

```powershell
.\Build.ps1 -VcpkgRoot C:\src\vcpkg -Configuration Release
```

Tests are opt-in. Pass `-BuildTests` to compile and package the UWP test target
alongside the sample.

With the default build directory, the generated package is under
`build/Frontend/RPCS3-UWP-Host/AppPackages/RPCS3UwpApp/<version>_x64_Test`.
The development manifest targets `Windows.Universal` and `Windows.Xbox` from
10.0.19041.0 through a tested maximum of 10.0.26100.0. It declares the restricted
`broadFileSystemAccess` capability plus `internetClientServer`,
`privateNetworkClientServer`, and `removableStorage`. Replace the sample identity,
publisher, logos, and capabilities before shipping it as another application.

To configure the same native UWP sample directly from this source folder, use:

```powershell
cmake -S . -B build-uwp -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build-uwp --config Release `
  --target RPCS3UwpApp
```

Enabling `UWP_IMGUI_FRONTEND_KIT_BUILD_TESTS` compiles and packages the test
target, but its raw WindowsStore executable is not a desktop test runner.
Configure the component separately as a desktop target to run the host-logic
tests:

```powershell
cmake -S UWP-ImGuiFrontend -B build-host-tests -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake `
  -DUWP_IMGUI_FRONTEND_IMGUI_DIR="$PWD/dependencies/imgui" `
  -DUWP_IMGUI_FRONTEND_BUILD_TESTS=ON
cmake --build build-host-tests --config Release `
  --target UwpImGuiFrontendTests
ctest --test-dir build-host-tests -C Release --output-on-failure
.\tools\VerifyCopies.ps1
```

For a consuming application, the folder's `vcpkg.json` requests Boost.Asio,
fmt, and RapidJSON. The Windows SDK supplies `windowsapp`, Windows Imaging
Component, DXGI, D3D12, and the legacy D3D compiler import libraries. A normal
integration supplies its own ImGui context and graphics backend; only the
portable sample carries the minimal ImGui source subset needed to run by itself.

## License

Original project-owned code is released under the Zero-Clause BSD license
(`0BSD`). It may be used, copied, modified, or distributed for any purpose,
including in proprietary and copyleft-licensed projects, without an attribution
condition from this project.

Bundled third-party software and assets retain their own licenses. See the kit
root's `THIRD_PARTY_NOTICES.md`, `dependencies/imgui/LICENSE.txt`, and
`UWP-ImGuiFrontend/assets/fonts/OFL.txt` before redistributing the kit.
