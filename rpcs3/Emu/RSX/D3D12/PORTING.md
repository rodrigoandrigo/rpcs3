# Embedded legacy D3D12 renderer

The renderer was imported from rpcs3-0.0.7 (22 source/header files and 37
minidx12 helper files). It is now adapted sufficiently to compile and link
into the Qt-free WindowsStore DLL, but remains EXPERIMENTAL. A successful
build is not proof of complete RSX compatibility or game execution.

## Build and activation

Run RPCS3-UWP/Build-Core.ps1 -ExperimentalD3D12, then Build-Host.ps1 without
-FrontendOnly. MSVC runs in the configured MSYS/UCRT64 environment.
The CMake option RPCS3_UWP_D3D12 defaults OFF; the switch enables it.
The normal core build retains Null rendering and returns UNSUPPORTED_RENDERER
from the graphics attachment API.

After initialize completes, the connected frontend passes its ID3D12Device
and DIRECT ID3D12CommandQueue to rpcs3_core_attach_d3d12 while stopped.
An attached experimental build instantiates D3D12GSRender at boot; an
unattached build retains NullGSRender. The internal legacy renderer setting
is still normalized to Null: there is no new Qt/configuration renderer enum.

The renderer does not create or present an HWND swapchain. Each flip resolves
the current display buffer into a new immutable RGBA8 offscreen texture and
publishes it after submission on the host queue. CPU-backed display buffers
are uploaded with their guest pitch. rpcs3_core_acquire_d3d12_frame returns
an owning COM reference and serial. The frontend creates an SRV in the
current swapchain slot and retains BOTH the texture and descriptor until
that slot's GPU fence completes. It displays the frame in the RPCS3 video
ImGui window. Only the frontend owns CoreWindow, swapchain, resize and Present.
The retired D2D/D3D11On12 overlay source is excluded from the DLL build.

## Adapted code

- Vertex input uses the current GRAPH frontend's interleaved analysis,
  memory requirements, raw persistent/volatile streams and 32-word layout
  descriptors. HLSL decodes types, byte order, frequency divisors/modulo,
  missing components and indexed/immediate streams. Expanded legacy primitives
  retain an explicit 16-bit range guard. Current draw-clause passes execute
  their pipeline dependencies and call thread::end once.
- Both HLSL stages share a checked 1136-byte constant layout, in a 1280-byte
  CBV allocation. It contains the current 48-byte texture units and raw vertex
  metadata. Packed clip planes and current fog modes are adapted; texture
  alpha kill occurs at the texture instruction. Half packing now uses the
  actual SM5 f32tof16/f16tof32 intrinsics.
- Current RSX pipeline analysis and sampled-image descriptors replace removed
  legacy retrieval APIs. Pipeline cache ownership/callbacks, shader IDs,
  constant offsets, MRT/depth attachment formats and restart cut values are
  integrated. HRESULT handling no longer shadows an argument named hr.
- Render targets use current framebuffer layout/state flags and all four
  depth formats. Sparse/unbound attachments are skipped. Readback uses SDK
  GetCopyableFootprints, allocated staging resources, per-plane copies,
  fence completion and tight CPU rows, not guessed pitches.
- Color/depth guest writeback uses destination pitch, big-endian components,
  separate stencil and RSX e4m12/f24 conversions. Removed empty download
  stubs no longer report success with no data.
- Sampling a writable attachment creates a retained GPU snapshot. Surface
  SRVs use the actual physical resource format. Texture-cache lookups return
  owning snapshots instead of unlocked map pointers; mutation is locked and
  overlapping protection faults conservatively invalidate the whole cache.
- Current label-release callbacks flush/wait on the RSX worker before the
  common processor writes the guest semaphore; acquire waits submit pending
  work. Device-removal HRESULTs are checked during producer fence waits.

## Evidence (2026-09-30)

Test-Embedding.ps1 -CompileIntegration -CompileLegacyD3D12 completed successfully:

- C ABI header, asynchronous host-runtime, broker path and CNG/DRBG tests passed.
- All 16 restricted integration probes and all 12 imported D3D12 translation
  units compiled. The linked renderer uses 11 units, excluding the retired
  desktop overlay.
- Real D3DCompile/D3DReflect verified all 16 cbuffer variables, the raw vertex
  decoder's generated HLSL, and six packed clip planes.
- Streaming-copy guards/tails passed lengths 0..512 and all 16 alignments.
- Actual WARP GPU clears/copies/readback passed RGBA8, D16, D24S8, D32 and D32S8
  on 17x3 surfaces with padded rows. Depth and stencil were checked separately;
  Z16F e4m12 conversion was checked. Shared attachment/publication, acquired COM
  ownership and detach lifetime checks also passed.

The updated experimental Release x64 core linked at
build-uwp-msvc/core/bin/rpcs3-core.dll (21,391,872 bytes). The connected host
rebuilt its x64 MSIX (10,849,856 bytes). Both DLL copies have SHA256
463E4AB59D2865B1A78397C9721174052126A5926C9C18E96677BD2DFB1E44EA.
Audit-Uwp.ps1 reported STATIC_IMPORTS_PASS and zero findings. Logs:
build-uwp-msvc/core-d3d12-msaa-build.log, host-d3d12-msaa-build.log,
d3d12-textures-msaa-tests.log and d3d12-msaa-audit.log. The detailed
audit is uwp-audit.json. Surface-range regression checks include zero length,
exclusive endpoints, the 4GiB boundary, detachment and retained ownership.

## Remaining limitations and evidence boundaries

No installed AppContainer/frontend presentation, PS3 game, Xbox or WACK run
was performed. Native WARP component tests are NOT UWP/Xbox runtime evidence
and do not exercise the entire shader/decompiler/draw path with a guest program.

Vertex textures now have four dedicated vertex-stage SRV/sampler slots, shared
CPU uploads/cache and writable-attachment snapshots. TXL uses SampleLevel at
LOD zero, matching the current vertex decompiler contract. 1D/2D/3D/cube
declarations compile; the native GPU probe exercises 2D vertex-stage sampling.
Coordinate scaling includes unnormalized coordinates and surface dimensions.
Vertex samplers use s0-s3 with vertex visibility (SM5 only permits 16 sampler
registers per stage), independently of pixel-stage s0-s15.

Native 2x/4x surfaces, TEXTURE2DMS attachment views, sample-count/mask PSO keys,
alpha-to-coverage, per-sample color/depth/stencil extraction and presentation
color resolve are implemented. Sampling/readback expands samples to 2x1/2x2
grids rather than averaging them. Expansion PSOs are reused per device/format;
temporary descriptors/resources survive until their producer fence completes.
Supported sample counts/formats are checked, not silently reduced to 1x.
WARP tests cover all five formats above at both sample counts, distinct
per-sample color values, a real vertex-stage texture sample, cached expansion
pipelines and debug-layer error checks. Programmable diagonal, centered-square
and rotated-square candidate patterns now replace the quality-zero rasterizer
pattern. OPTIONS2/GraphicsCommandList1 support is required for MSAA; unsupported
devices fail explicitly instead of silently approximating the request. The
pattern is restored after command-list resets and single-sample helper passes,
and depth transitions/readback use the pattern under which depth was written.
WARP Tier 2 coverage probes check three vertical and three horizontal edge
thresholds against every programmed sample, including both four-sample patterns.
These tests prove the supplied D3D12 coordinates, NOT exact NV47/PS3 sample
positions/index order: the table remains a geometric candidate without an
independent PS3 fixture. Alpha-to-one, disabled-MSAA coverage behavior and
guest-game correctness are not validated or complete.

Programmable positions cannot be tested using EvaluateAttributeAtSample or
HLSL SamplePos: those results are undefined for custom positions in the
[D3D12 specification](https://microsoft.github.io/DirectX-Specs/d3d/ProgrammableSamplePositions.html).
Coverage probes use actual geometry instead. Shader interpolation equivalence
to RSX, including centroid priority, is not established by these probes.

Texture cache lookups no longer skip CPU invalidation when texture registers
are unchanged. Pitch, linear/swizzled layout and dimensionality participate
in cache compatibility; initial uploads protect memory before reading it.
Every prepared linear surface now has its own page watch, independently of
texture-cache protection. A shared page arbiter keeps the strongest overlapping
permission: removing a texture watch cannot expose a dirty GPU surface. GPU
writes make pages inaccessible; CPU read faults request renderer-thread
readback and downgrade pages to read-only. CPU write faults first preserve GPU
bytes, invalidate/retire the GPU surface, remove its watch and retry the CPU
instruction. The faulting CPU releases its VM lock; DMA-offloader faults expose
the existing recovery flag while waiting. Explicit read/write barriers use the
same synchronization. Unmapping retires watches/resources; restoring original
permissions does not grant write access to logically read-only VM pages.

New/replacement attachments import guest RAM before partial draws/clears.
The inverse MSAA pass restores individual color/depth/stencil samples; stencil
uses per-value replacement passes, not optional shader stencil-reference export.
Guest rows use framebuffer actual pitches and privileged writes preserve row
padding. Layout changes conservatively flush/discard old surfaces before import
to avoid unordered old/new aliases. This is correctness-first, not optimized.
Simultaneous overlapping attachments, swizzled rasters and tiled regions are
explicitly rejected rather than reading/writing them as linear data.

Native tests cover shared-page ownership and real NOACCESS/read-only CPU faults
with a worker and shared guest/privileged aliases. The latter uses a stand-in
readback, not RPCS3's VM dispatcher. Separate WARP tests round-trip distinct CPU
sample data through color, D16, D24S8, D32 and D32S8 at 2x/4x, including separate
stencil and custom-pattern depth transitions, without debug-layer errors.

Privileged CPU/SPU accesses now use scoped renderer-thread leases: GPU writes
are read back before access, cached textures/surfaces are invalidated for writes,
and drawing cannot resume until all granted leases end. Nested requests remain
serviceable while another lease is active. This covers RSX reservation guards
(including batched DMA), VM reservation/light atomic operations and SPU live
reservation comparisons. Guards precede reservation/VM locks; event comparisons
release their lease before waiting. Other backends retain their existing behavior.
Native queue tests exercise nested leases, cancellation, failed synchronization
and privileged-alias writes, but do not validate guest scheduling or performance.

Vulkan/OpenGL are the implementation reference for deferred fault readback and
VM temporary unlocking (VKGSRender.cpp/GLGSRender.cpp). Their shared shaders in
Program/MSAA use sample_index = x + y * sample_count.y; the supported 2x1/2x2
grids match D3D12 expansion/restoration. This confirms renderer convention, not
PS3 physical sample coordinates or hardware lane order. No PS3 capture or real
console is available for that independent comparison.

Fault handling also follows their memory-manager flush and ZCULL fallback:
unowned read/write faults must reach the report controller rather than being
rejected prematurely. Write synchronization invalidates texture-only ranges
as well as attachments, including renderer-local write barriers (not just
queued CPU requests). These paths compile; component tests do not establish
live ZCULL/report or game correctness.

This is still NOT complete CPU/GPU coherence. No running
guest has exercised the full fault/VM/offloader/teardown integration. Interior,
tiled and format-reinterpreted aliases, bit-preserving exceptional floating
depth values and exact PS3 sample fixtures remain incomplete/unverified.
Complete modern shader semantics, conditional/ZCULL rendering and full
primitive-barrier compatibility are also not validated or complete.
The inherited fixed upload/descriptor capacities and 16-bit non-native
primitive expansion impose limits; these are not claims of full current
RPCS3 renderer parity. Shared-frame lifetime is tested at component level,
not through an installed app. Do not describe this as a finished UWP emulator.
