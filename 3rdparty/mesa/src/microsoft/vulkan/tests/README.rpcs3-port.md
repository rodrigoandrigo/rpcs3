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
