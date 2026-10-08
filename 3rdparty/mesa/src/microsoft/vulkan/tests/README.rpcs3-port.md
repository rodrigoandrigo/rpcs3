# Dozen source port into the RPCS3 Mesa tree

Reference: `C:/Users/rodri/Dev1/Projetos/mesa-modificado`, Mesa 26.2.2,
round42. Destination: the RPCS3 vendored Mesa 26.2.2. Date: 2026-10-07.

This is a source-level Dozen improvement, NOT RPCS3 Vulkan integration.
The RPCS3 build/renderer selector/package still use their existing configuration.
Dozen's standalone build lives in `build-uwp-msvc/mesa-dozen-port`, outside the
existing Gallium build. No application installation or package replacement is
part of this work.

## Ported scope

The reference adds 56 extension-table fields relative to the original Dozen
in this checkout. Their implementations, shared SPIR-V/NIR/DXIL dependencies,
common Vulkan runtime changes and applicable WSI code were ported together.
Capability-dependent fields retain their D3D12 probes. Desktop HDR remains
excluded by `_XBOX_UWP`. The source includes push descriptors/templates,
fragment barycentrics, calibrated timestamps and their KHR alias, image
robustness, shader extensions, rendering/descriptor/queue aliases, and
instrumentation extensions. Registry declarations alone are not considered
implementations.

`VK_EXT_robustness2` remains partial: only `robustBufferAccess2` is reported,
conditional on root signature 1.1; `robustImageAccess2` and `nullDescriptor`
remain false. After the seven local additions below, the 133 pending candidates remain
unadvertised. They are not included in the claim of source parity.

The shared robust-image lowering also handles bindless image size/sample
queries and does not dereference an unknown format description. Windows QPC
calibration converts the deviation bound to nanoseconds, rather than counter
ticks. UWP-specific source guards in the destination are preserved.

The first Windows build caught an invalid `nir_def::parent_instr` access in
the imported barycentric sample lowering. It now uses the current NIR accessor
`nir_def_as_intrinsic`; the source regression test also checks this fix.
The memory address-binding report also uses `dzn_ID3D12Resource_GetDesc`:
the direct COM macro has a different struct-return ABI under MSVC.

The native compute probe then reproduced a null generic binding lookup and a
root-signature mismatch. DXIL now looks up SSBO access within the typed SSBO
namespace, retaining writable aliases. Bindless lowering explicitly removes
fully lowered resource declarations before inserting read-only descriptor
table SSBOs, so stale UAV declarations cannot leak into the shader signature.
Image sample-count intrinsics are lowered to bindless handles as well.

DXIL validation initializes its error output, checks COM call results before
dereferencing them and extracts bounded error text without requiring the
optional `dxcompiler.dll`. UTF-16 errors are converted to UTF-8; validator-owned
error buffers are not modified. Dozen forwards UWP validation failures through
the existing Mesa callback and preserves failed compute PSO HRESULTs in logs.

## Verification tiers

Run static support tests from the RPCS3 repository root:

```powershell
C:/msys64/ucrt64/bin/python.exe -X utf8 -B -m unittest discover -s 3rdparty/mesa/src/microsoft/vulkan/tests -p 'test_*support.py'
C:/msys64/ucrt64/bin/python.exe -X utf8 3rdparty/mesa/src/microsoft/vulkan/tests/extension_support_scaffold.py --check
```

UTF-8 mode is required on Windows because the imported sources contain Unicode.
The scaffold script regenerates its manifest; do not hand-edit generated JSON.
These tests check source structure and reporting policy, not Vulkan behavior.

`dozen_port_smoke.cpp` is a standalone native-PC probe, not a shipped core
component. Compile with MSVC C++20, `/EHsc /MD`, and Mesa's `include` directory.
Link `dxguid.lib` and `user32.lib` for the native-only diagnostic/hidden WSI probe.
Pass absolute paths to the standalone `vulkan_dzn.dll`, the SDK x64 `dxil.dll`
and the assembled compute fixture. Its dependent UWP VC runtime must be
discoverable. The native PC probe preloads the validator/system D3D12 module
because it has no package identity; the driver's packaged loading stays intact.
It accesses the ICD directly without installing it or changing Vulkan
loader/registry configuration.

```powershell
C:/msys64/ucrt64/bin/spirv-as.exe --target-env vulkan1.2 3rdparty/mesa/src/microsoft/vulkan/tests/dozen_port_smoke.spvasm -o build-uwp-msvc/dozen-port-smoke.spv
C:/msys64/ucrt64/bin/spirv-val.exe --target-env vulkan1.2 build-uwp-msvc/dozen-port-smoke.spv
```

The assembly fixture adds a null Workgroup initializer to the GLSL-generated
module and declares `SPV_KHR_zero_initialize_workgroup_memory`. Compiling the
GLSL alone does NOT exercise this feature. The probe enables the corresponding
Vulkan feature, creates a compute pipeline and dispatches through a push
descriptor. It verifies 1,024 GPU-written words, including a read of zeroed
shared memory in each workgroup.

The probe checks physical-device/extension discovery, conservative partial
feature reporting, creation with all advertised device extensions, selected
command entrypoints, transfer/compute-to-host synchronization and GPU readback.
It exercises calibrated timestamps only when supported by the adapter.
The default compute mode does not test graphics or WSI. No mode proves all
extension semantics, shader conformance, installed UWP/AppContainer execution,
RPCS3 games, or Xbox.

Use `--bounded` before the three paths to enable `robustBufferAccess2` and
exercise the descriptor-table path. This restricts the descriptor to 2,048
bytes inside a 4,096-byte allocation: 512 out-of-range GPU stores must leave
the allocation's sentinel values intact. It is a focused buffer write-bounds
test, not full robustness2 conformance. `--debug` launches only this probe as a
child debugger target to capture D3D12 messages; it does not attach to RPCS3.

Observed on the native PC AMD Radeon RX 6600M: both bindless and bounded compute
probes passed, including push descriptors, Workgroup zero initialization,
readback and calibrated timestamps. The standalone driver built with
`uwp=true`, Store CRT and `/APPCONTAINER`. This is NOT installed UWP execution.
The retail/UWP driver's debug-layer suppression was preserved in the final
build; diagnostic activation used during investigation was removed.

Build, source-test and probe logs are kept under `build-uwp-msvc/dozen-port-*`.
Windows/D3D12 build and smoke results must be reported separately from source
tests. No Vulkan CTS conformance claim follows from these checks.

## Seven additional local extensions

These are backend implementations, not just new extension-table entries:

| Extension | Mapping / reporting boundary |
| --- | --- |
| `VK_EXT_4444_formats` | Exact packed DXGI mappings. A4R4G4B4 enabled; A4B4G4R4 requires native texture and sampling support. |
| `VK_KHR_present_id` | Tracks successfully submitted IDs; zero does not advance the ID. No `present_wait` or completion guarantee. |
| `VK_KHR_swapchain_mutable_format` | Castable application images and typed private DXGI blit targets; UNORM/sRGB views. |
| `VK_EXT_conservative_rasterization` | Hardware-tier probe and overestimate PSO state. No underestimate, extra overestimation, conservative points/lines or fully-covered input. |
| `VK_EXT_extended_dynamic_state` | Dynamic topology, culling, front face, depth/stencil, viewport/scissor counts and vertex strides. Requires independent front/back stencil masks and references. |
| `VK_EXT_extended_dynamic_state2` | Discard, depth-bias enable and restart. Optional dynamic logic-op and patch-control-point features remain false. |
| `VK_EXT_vertex_input_dynamic_state` | Attributes/bindings/divisors, late vertex-format shader conversion and per-draw vertex views. |

PSO variants use canonical masked keys and a lock; selection never changes the
shared pipeline's base PSO. Retained vertex NIR owns its compiler options. The
implicit point-fill GS reads dynamic culling/front-face/depth-bias-enable from
the runtime CBV rather than baking ignored creation values into its shader.
The underlying point-fill path still has its inherited unsupported slope-scaled
depth-bias limitation; this is not full graphics conformance certification.

The mutable WSI probe also uncovered and fixed a dedicated-allocation image
mismatch, RGBA/BGRA copy mismatch, and enhanced-barrier access/discard issues.
Private DXGI images remain typed; no frontend attachment is performed.

Additional native fixtures for `--seven` mode:

```powershell
C:/msys64/ucrt64/bin/glslangValidator.exe -V --target-env vulkan1.2 3rdparty/mesa/src/microsoft/vulkan/tests/dozen_seven_smoke.vert -o build-uwp-msvc/dozen-seven.vert.spv
C:/msys64/ucrt64/bin/glslangValidator.exe -V --target-env vulkan1.2 3rdparty/mesa/src/microsoft/vulkan/tests/dozen_seven_smoke.frag -o build-uwp-msvc/dozen-seven.frag.spv
# Prepend --seven to the same three absolute driver/validator/compute paths.
```

Native RX 6600M results: 12 graphics readback cases (discard, dynamic attribute
offsets/bindings, scaled vertex conversion, conservative PSO and point-fill
culling/front-face changes), exact clear/readback of both packed 4444 formats,
mutable UNORM/sRGB views and accepted DXGI presents with IDs 1, 0, 2. The callback
accepts an isolated composition swapchain, NOT visible frontend presentation.
Point-fill depth-bias enable is exercised without a depth attachment, not a
numerical bias test.

Final standalone driver: Store CRT, `/APPCONTAINER`, no forced diagnostic layer.
No RPCS3 build selector, frontend or package changes. Source suite: 254 tests,
one skipped. Native compute regressions: bindless and bounded. Logs are under
`build-uwp-msvc/dozen-seven-*` and `dozen-port-*`. UWP/Xbox execution and Vulkan
CTS remain unverified.

Reviewed matrix: 175 candidates, 42 implemented, 133 pending, zero pending
advertised. Four of these seven were outside the original 171-candidate matrix;
three moved out of its pending list.

## Follow-up: six requested extensions (partial implementation)

This follow-up does NOT complete all six extensions. Four are exposed with the
following contracts:

- `VK_EXT_conditional_rendering`: native draw/dispatch and explicit attachment
  clears are predicated; load-op clears, copies, barriers and internal work are
  not. The 32-bit Vulkan predicate at a four-byte offset is snapshotted into a
  zero-extended, eight-byte D3D12 predicate. Inherited conditional rendering is
  unsupported and reported false.
- `VK_KHR_workgroup_memory_explicit_layout`: explicit/scalar layouts and 8/16-bit
  Workgroup accesses use the existing explicit TGSM lowering. The GPU fixture
  aliases 8/16/32-bit blocks over the same storage, checks zero initialization,
  subword values and synchronization across 32 invocations per group.
- `VK_KHR_shader_float16_int8`: Int8 ALU is widened in NIR and no longer depends
  on native 16-bit hardware. Float16 remains gated by Native16BitShaderOpsSupported;
  8-bit storage features keep their independent original gate. The GPU fixture
  verifies Int8 wraparound and Float16 arithmetic.
- `VK_EXT_provoking_vertex`: native FIRST_VERTEX only; LAST_VERTEX is rejected,
  `provokingVertexLast=false`. Generic LAST support for lines, integer varyings
  and arbitrary geometry shaders is NOT implemented. No XFB ordering guarantee.

Two implementations remain experimental and are NOT advertised:

- `VK_EXT_transform_feedback`: SO declarations, four buffer bindings, 32-bit
  counters, resume and primitive-capacity handling were tested on the native GPU.
  Padding and counter-adjacent sentinels were preserved. Dynamic discard now
  changes the SO rasterized-stream field. Remaining blockers: capture original
  shader position before Y/Z conversion and preserve capture through the
  point-fill emulation GS. Queries, multi-streams and byte-count draw are false.
  The final driver reports `transformFeedback=false`.
- `VK_KHR_present_wait`: Win32 tracks accepted IDs separately from completion
  and associates IDs with DXGI present sequence numbers. Wait uses displayed
  frame statistics after the GPU timeline, preserving timeouts/errors. The
  visible native composition probe timed out: GPU completion succeeded but DXGI
  PresentCount stayed zero. IDXGISwapChainMedia is desktop-only in the SDK and
  was NOT introduced into the UWP driver. `presentWait=false`; completion is
  not faked with QueueWaitIdle or the latency handle.

Build the compute fixture with `spirv-as --target-env vulkan1.2` from
`dozen_six_compute.spvasm`, and validate it with `spirv-val`. The XFB GLSL
fixtures compile with glslangValidator. Run `dozen-port-smoke.exe --six-core`
with the same three absolute paths as above, selecting `dozen-six-compute.spv`.
The old `--six` experimental test intentionally cannot enable the two disabled
features in the final driver; its failed presentation log is retained.

Final native RX 6600M verification: 20 graphics readback cases (including all
four normal/inverted draw conditions and four explicit attachment clears), two
packed formats, three accepted presents, and 1,024 checked compute words.
Source support suite: 261 tests, one skipped. Matrix: 176 candidates, 46
implemented, 130 pending, zero pending advertised. Logs: `dozen-six-*` in
`build-uwp-msvc`. This is native-PC evidence, NOT UWP/Xbox execution or Vulkan CTS.
The Store-CRT/APPCONTAINER driver has no forced debug layer. No RPCS3 selector,
frontend, package, installation or ICD registry changes were made.

## Follow-up: robustness, maintenance5, bias control and uint8 (partial)

This request is NOT fully complete. The following two extensions are exposed:

- `VK_EXT_pipeline_robustness`: resolves device, pipeline and per-stage state
  through vk_pipeline_robustness_state_fill, including shader-stage overrides.
  Graphics and compute hashes include the resolved robustness state; image NIR
  lowering uses that state rather than only the device feature. Devices enabling
  pipelineRobustness use bounded descriptor tables even when device-wide
  robustBufferAccess2 is disabled, so per-pipeline robustness2 cannot fall back
  to a whole-buffer bindless view. Native buffer/vertex robustness can be stronger
  than a DISABLED request. robustImageAccess2 remains unsupported.
- `VK_EXT_depth_bias_control`: supports only LEAST_REPRESENTABLE_VALUE_FORMAT,
  with inexact native bias. Static rasterization pNext and CmdSetDepthBias2EXT
  consume the representation; unsupported representations/exact requests return
  an error rather than silently changing their meaning. Optional forced UNORM,
  FLOAT representation and depthBiasExact are false. Existing native bias/PSO
  and emulated point runtime-state paths are reused.

`VK_KHR_maintenance5` remains unadvertised: CmdBindIndexBuffer2KHR now respects
an explicit range or VK_WHOLE_SIZE; GetImageSubresourceLayout2KHR delegates to
the native footprint calculation; GetDeviceImageSubresourceLayoutKHR constructs
an unbound temporary image and releases it after querying. Remaining work:
complete/validate A8 and A1B5G5R5 support, maintenance5 properties and the other
extension contracts. These helpers alone are not extension support.

`VK_EXT_index_type_uint8` remains unimplemented/unadvertised (including its KHR
alias). Native D3D12 index buffers only use the existing 16/32-bit paths. Required
work includes GPU widening, restart conversion, range limits, direct/indirect
draws, triangle-fan lowering and synchronization when the source is rewritten.
No CPU readback workaround or fabricated feature bit was added.

Native test: `dozen-port-smoke.exe --four-core` with the original compute SPIR-V
and the same absolute driver/DXIL paths. It enables pipelineRobustness without
device-wide robustness2, requests DISABLED storage robustness at pipeline level
and ROBUST_BUFFER_ACCESS_2 at shader level, then checks 512 written values plus
512 untouched sentinels beyond the descriptor range. Twelve graphics readbacks
also exercise CmdSetDepthBias2EXT with explicit native-format representation;
there is no numerical depth-attachment bias validation yet. Image robustness
override/cache wiring is source-tested, not a GPU image-OOB conformance test.

Source support suite: 266 tests, one skipped; scaffold suite: six tests.
Matrix: 176 candidates, 48 implemented, 128 pending, zero pending advertised.
MSVC standalone build/link passed. Logs: `build-uwp-msvc/dozen-four-*`.
Native GPU verification is not UWP/Xbox or Vulkan CTS. No RPCS3/frontend/package
integration, installation or registry changes.

## Current follow-up: presentation completion, maintenance5 and uint8 indices

This section supersedes the historical partial status above. The Dozen build
remains isolated; RPCS3-UWP/Build-Mesa.ps1 and frontend/package activation are
unchanged.

- `KHR_present_wait` is enabled on Windows. The swapchain uses the UWP-compatible
  DXGI frame-latency waitable object and maximum frame latency one. It consumes
  the initial ready token before the first Present and serializes later presents
  so that only one recorded presentation is outstanding. ID-zero frames are
  tracked too. Completion is not inferred from a GPU fence or QueueWaitIdle.
  Polling checks device removal, errors and the original timeout deadline.
  Native composition tests passed IDs 1, 0, 2, displayed waits, repeated completed
  zero-time waits and a zero-time timeout for an unsubmitted ID.
  This does not provide an exact scan-out timestamp or Xbox validation.
  See Microsoft's [waitable swapchain guidance](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains).
- `KHR_maintenance5` is enabled. A1B5G5R5 sampling views swap R/B against native
  B5G5R5A1; clears preserve Vulkan packed bytes. It advertises sampling/transfer,
  not unsupported render-target/storage access for this swizzled format. A8 uses
  native A8 images and advertises no buffer-view support. Properties explicitly
  describe native depth/stencil ONE swizzles, fixed point size one and conservative
  sample-count ordering. Common Vulkan runtime handles inline shader modules,
  pipeline flags2 and version-filtered proc lookup. Buffer flags2 and restricted
  buffer-view flags2 are consumed in Dozen. Native tests passed both new packed
  clear/readback formats, object/objectless linear subresource layout agreement,
  index-buffer range binding, buffer/view flags2 and API 1.2 core-name filtering.
  The image sampling swizzle and inline-module/PSO flag paths are source-reviewed,
  not CTS validated.
- `EXT_index_type_uint8` and its KHR alias are enabled. Compute widens bytes to
  uint32 indices, including unaligned source offsets; FF becomes FFFFFFFF only
  when primitive restart is enabled. Conversion happens before each indexed
  direct/indirect draw, not at bind time. It preserves pipeline state and restores
  source/destination barriers; triangle-fan lowering consumes widened indices.
  Logical buffer size is no longer confused with native four-byte allocation
  padding. Native GPU readbacks passed direct, indirect, repeated draw following
  a GPU source update after bind, restart strips, index 255 without restart, and
  triangle fan. Converted views beyond UINT32_MAX bytes fail allocation rather
  than silently truncating the range. Large 2D dispatch limits and legacy barrier
  adapters still need broader hardware/CTS coverage.

`EXT_transform_feedback` is **still unadvertised**. New NIR code mirrors masked
position stores to an XFB-only generic varying before clip/viewport conversion;
the synthetic point GS copies XFB metadata and captures culled primitives while
placing only raster positions outside the clip volume. These new paths compile,
but have not passed a native XFB position/point test. Moreover native point SO
can partially capture an original triangle when the remaining buffer fits only
one or two points. A GPU primitive-atomic capture path remains necessary; merely
rounding the buffer size does not handle arbitrary GPU resume counters. This
follow-up therefore does **not** complete the user's entire four-item request.
The Vulkan [transform feedback contract](https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html)
is the reference for the remaining primitive-order/capacity work.

Verification: MSVC Store/APPCONTAINER DLL build/link passed. Source suite:
270 tests, one skipped; scaffold suite: six passed. Matrix: 177 candidates,
52 implemented, 125 pending, zero pending advertised. Native logs:
`build-uwp-msvc/dozen-contracts-smoke.log`, `dozen-present-complete-smoke.log`,
`dozen-complete-build.log`, and `dozen-complete-support-tests.log`.
No Vulkan CTS, installed UWP/Xbox run, package build or integration was performed.

### Provoking vertex LAST follow-up (2026-10-07)

The previous FIRST-only limitation is superseded: `provokingVertexLast` is now
enabled and both pipeline modes are accepted. Generated passthrough geometry
shaders select the flat integer/float payload without changing positions or
smooth interpolation. Application geometry shaders delay emission by one/two
vertices for line/triangle strips, flush explicit and implicit strip tails, and
retain their original output vertex limit. Point expansion also selects the
requested payload. Triangle fans use the consistent emulated index ordering
when this feature is enabled. Restarted indexed triangle strips are converted
on the GPU to triangle lists, resetting winding parity at restart boundaries;
PrimitiveId parity alone cannot identify these boundaries.

The shared indirect restart path also fixes command-buffer allocation stride,
64-bit destination-address construction, and nonzero first-index end bounds.
Pipeline and DXIL cache keys distinguish provoking mode and fan emulation.

Native `dozen-port-smoke --provoking` passed 18 FIRST/LAST cases: triangle lists,
strips, fans, lines, indexed indirect draws, polygon points, application triangle
and line GS, and indexed primitive restart. Tests check integer/float agreement,
coverage equivalence and smooth interpolation where applicable, including cache
reuse. MSVC Store/APPCONTAINER build/link passed, as did native `--four-core`,
`--six-core`, `--contracts`, and `--present` regressions. Evidence:
`build-uwp-msvc/dozen-provoking-smoke.log` and
`build-uwp-msvc/dozen-provoking-regression-*.log`.

This is not Vulkan CTS certification. XFB preservation properties remain false
and transform feedback remains disabled; synthetic-stage pipeline-statistics
accounting and UWP/Xbox runtime coverage are not established by these tests.
No RPCS3/frontend integration or package changes were made.
