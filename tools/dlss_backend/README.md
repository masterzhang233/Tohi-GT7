# External NGX Vulkan backend

This independent Windows x64 target records DLSS SR/DLAA into a **borrowed host
Vulkan device and command buffer**. Its standalone tests exercise synthetic inputs
or captured GT7 inputs; these tests do not establish in-game integration, image
quality or performance.

`kyty_dlss_backend.h` is the complete version 1 C ABI. It includes Vulkan C types
and no NGX headers. The DLL alone includes/links the externally installed NVIDIA
SDK. The smoke host is built with `/MD`, the DLL with `/MT`; no C++ objects or CRT
allocations cross that boundary. SDK/runtime files are neither downloaded nor
copied by this target. NVIDIA's SDK/runtime terms still apply; a C ABI is a
technical boundary, not a licensing determination.

## Build and run

The host's device-extension query also records a successful SR feature-support
query before `vkCreateDevice`. Streamline initializes NGX during device creation;
some driver/SDK combinations then return `NotImplemented` to the first native SR
support query. Only that result may reuse earlier affirmative evidence for the
same instance, physical device and canonical runtime path. Other failures or
unsupported flags invalidate the cached evidence. NGX initialization, loaded DLL
identity, capability and driver checks still run when creating the SR session.
`kyty_dlss_feature_support_tests` covers this policy without loading NGX or a GPU.
This addresses the observed query-order failure; coexistence with live FG and its
visual/performance behavior still require runtime verification.

Use a Visual Studio developer shell with CMake/Ninja and MSVC or clang-cl:

```powershell
cmake -S tools/dlss_backend -B C:/work/dlss-backend-build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DKYTY_NGX_SDK_ROOT=C:/external/DLSS
cmake --build C:/work/dlss-backend-build
C:/work/dlss-backend-build/kyty_dlss_backend_smoke.exe `
  C:/work/dlss-backend-build/kyty_dlss_backend.dll `
  C:/external/DLSS/lib/Windows_x86_64/rel C:/work/dlss-backend-smoke
```

The SDK checkout must contain `include/nvsdk_ngx_vk.h` and
`lib/Windows_x86_64/x64/nvsdk_ngx_s.lib`. The runtime directory must contain
`nvngx_dlss.dll`. All smoke arguments and backend runtime/log paths are absolute.
The DLL checks that a loaded `nvngx_dlss.dll` is the file in the requested runtime
directory. The host should also load the backend DLL by an absolute path with
restricted DLL search flags, as the smoke does.

The smoke creates its own Vulkan instance/device/resources, loads only the C ABI,
and records four jittered synthetic 320x180 frames into a persistent 640x360
Performance/Preset K feature. It checks extension/ABI/format/unsafe-destroy
rejections, first-frame-only creation/reset, and every output RGB component for
finite, nonempty, nonconstant data after a NaN clear. Alpha is not checked because
NGX alpha upscaling is disabled. `smoke-report.json` and four RGBA16F payloads are
written to the output directory. The smoke waits on host-owned fences for
readback; the backend never submits work or waits. Validation layers are not
enabled by this smoke.

Add `--quality-1440p` to run the same four-frame checks at fixed 1920x1080 input
and 2560x1440 output with the Quality hint and Preset K. This is a custom 75%
input ratio, not the runtime's standard Quality recommendation. The report lists
the actual input, recommendation, supported minimum/maximum and range membership.
Production backend dimensions and Quality selection already use the existing ABI;
this option changes only the diagnostic host.

## Explicit DLSS 4.5 model selection

The existing ABI and default **Preset K** behavior are unchanged. To request the
DLSS 4.5 second-generation transformer on an RTX 5080, set this process environment
variable before creating the backend context:

```powershell
$env:KYTY_GT7_DLSS_PRESET = 'M'
C:/work/dlss-backend-build/kyty_dlss_backend_smoke.exe `
  C:/work/dlss-backend-build/kyty_dlss_backend.dll `
  C:/external/DLSS/lib/Windows_x86_64/rel C:/work/dlss45-smoke --quality-1440p
```

Accepted values are exactly `K`, `M` and `L`. Unset/empty selects K; any other value
returns `INVALID_ARGUMENT` before initializing NGX. The value is captured once
per context; changing it requires GPU completion and context recreation. It does
not alter input/output dimensions, quality hint, jitter, motion, exposure or
history handling. The existing 1920x1080 -> 2560x1440 custom 75% ratio remains
fixed, with a Quality hint. There is no automatic fallback from M/L to K, because
that would conceal failure to activate the explicitly requested model.

NVIDIA identifies M and L as DLSS 4.5 models. M is optimized for Performance and
L for 4K Ultra Performance, but both support Quality/Balanced/DLAA. We choose M
for the explicit 5080 trial at the existing fixed ratio, **not** as a claim that
it is NVIDIA's recommended Quality preset. K remains the default, particularly
for RTX 20/30 cards that lack native FP8 support. See the official
[NVIDIA DLSS 4.5 FAQ](https://www.nvidia.com/en-us/geforce/forums/geforce-graphics-cards/5/580689/dlss-45-super-resolution-faq-updated-jan-14/)
and [SDK preset definitions](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs.h).

An explicit request fills all six official ratio-specific preset hint slots.
Setting only the Quality slot was insufficient in the 75% custom-ratio test:
M produced the same GPU output as K. Filling every slot made selection affect
GPU output and the runtime log acknowledged the M hints. The default unset path
retains the original single quality-mode K hint.

The SDK must expose the named M/L enums, and the supplied runtime must support
them; the verified local runtime is 310.9.16.0. Its version alone does not mean
that the second-generation model is selected. After the first successful
evaluation recording, the backend prints the requested preset and writes
`dlss-backend-model.json` in its NGX log directory. This is **request evidence**,
not an effective-model/GPU-completion claim: the public NGX interface used here
has no effective-preset query, and NVIDIA App/driver overrides may apply. The
NVIDIA overlay's DLSS statistics are the external effective-model check. The
smoke report likewise keeps `effective_preset_verified: false` and verifies
that an invalid preset is rejected. A successful smoke only verifies the GPU
record/submit/readback path and persistent history contract; it does not establish
in-game quality, frame generation or performance.

Set `KYTY_GT7_DLSS_NGX_LOG=1` for NVIDIA's verbose callback and file logs during
diagnosis (off by default). `validate_model_presets.ps1` runs default K, explicit
K, M and L in separate processes at fixed 1080p -> 1440p. It checks all four GPU
smokes, confirms explicit/default K agree, and rejects bit-identical K/M results
so a silently ignored model request cannot pass this regression. A differing
output is not an independent identification of a neural model. For example:

```powershell
./tools/dlss_backend/validate_model_presets.ps1 `
  -BackendBuild C:/work/dlss-backend-build `
  -RuntimeDirectory C:/external/DLSS/lib/Windows_x86_64/rel `
  -OutputDirectory C:/work/dlss-model-regression-new
```

To validate the renderer's actual input-conversion compute shader, compile
`src/graphics/host_gpu/shaders/gt7_dlss_prepare.comp` with `glslangValidator -V`
and supply the verified four-frame 1920x1080-to-3840x2160 `gt7.ngx-replay.v2`
sequence produced by `tools/dlss_probe/prepare_capture.py`:

```powershell
C:/work/dlss-backend-build/kyty_dlss_backend_smoke.exe `
  C:/work/dlss-backend-build/kyty_dlss_backend.dll `
  C:/external/DLSS/lib/Windows_x86_64/rel C:/work/dlss-prepare-check `
  --prepare C:/captures/ngx-sr-inputs/sequence.json C:/work/gt7_dlss_prepare.spv
```

The same prepare command also accepts a four-frame 1920x1080-to-2560x1440
sequence with `quality: "quality"`, selecting the Quality hint for that replay.

This mode uploads the captured raw B10G11R11 color, RG16F motion and depth-aspect
bytes, executes that shader, then compares every converted color/depth/motion/mask
element bitwise against the independent CPU converter. A mismatch aborts before
NGX submission for that frame. Matching GPU images are fed directly to the external
backend using one persistent feature across all four captures. The report records
per-resource mismatch counts and output finite-RGB checks. This mode enables
`shaderStorageImageExtendedFormats` for the RG32F/R8 storage outputs. Raw depth
bytes are uploaded as R32F, so this verifies numerical conversion rather than the
live guest depth view's lifetime/layout. Invalid motion stays a zero placeholder;
the verified mask is not passed to DLSS and the saved NGX images have no host
fallback composite. These are diagnostic outputs, not a quality acceptance result.

The independent compositor test uses the renderer's actual
`gt7_dlss_composite.comp`, compiled with `glslangValidator -V`:

```powershell
C:/work/dlss-backend-build/kyty_dlss_backend_smoke.exe `
  C:/work/dlss-backend-build/kyty_dlss_backend.dll `
  C:/external/DLSS/lib/Windows_x86_64/rel C:/work/dlss-composite-check `
  --composite C:/work/gt7_dlss_composite.spv
```

It creates synthetic HDR base/effect/native images and masks at 17x11 and 34x22,
covering alpha 0/1/intermediate and -0.25/1.25, signed HDR output, four edges/corners,
interior mask transitions, dispatch bounds and half rounding. An independent double-precision CPU reference
evaluates normalized pixel-center linear-clamp sampling, the clamped 2x2 invalid
footprint, and `(1-A)*base+P` without alpha/RGB saturation, or native fallback. Every RGBA value must be finite,
alpha must equal one, and each RGB component must be within one FP16 ULP of the
nearest-even reference. Reports include ULP histograms; CPU/GPU raw images are
saved. The one-ULP threshold applies to these fixtures, not every possible HDR
input. This mode does not evaluate NGX or run the game's effect fragment shaders.
Use `--composite-resized` instead of `--composite` to exercise 21x15 inputs,
28x20 reconstructed color and 42x30 guest composition, preserving the live
1080:1440:2160 ratios. The independent reference linearly samples reconstructed
color as well as the effect/native images and applies the same acceptance checks.

## Device creation diagnostic

`kyty_dlss_device_probe` isolates Vulkan device creation without starting a game,
creating an NGX feature, or submitting GPU work:

```powershell
C:/work/dlss-backend-build/kyty_dlss_device_probe.exe `
  C:/work/dlss-backend-build/kyty_dlss_backend.dll `
  C:/external/DLSS/lib/Windows_x86_64/rel `
  C:/work/device-config.json C:/work/device-report.json
```

Example configuration:

```json
{
  "api_minor": 3,
  "reservations": "guest",
  "emulator_extensions": true,
  "emulator_features": true,
  "emulator_identity": true
}
```

`api_minor` accepts 2 or 3. Reservations are `none`, `guest`, or
`early_cuda_guest`; the last option reports `cuInit`'s result separately.
`guest` reserves the emulator's low and extended address intervals with its
16 MiB minimum-free-region rule, using ordinary reservations rather than mapped
guest data/placeholders. Real backend instance/device extension queries run after
these reservations. `emulator_extensions` adds the supported extension-name
snapshot from the renderer. `emulator_features` reproduces the captured RTX 3080
feature requests and two-queue configuration, requires API 1.3 and emulator
extensions, and is a device-specific diagnostic rather than a portable feature
selector. `omit_extension` can remove one extension for a controlled comparison.
`emulator_identity` uses the renderer's Vulkan application/engine names and
versions. Reports retain loaded NGX/CUDA/Vulkan/RTSS module paths at each stage
and the actual `vkCreateDevice` result. The probe creates no SDL window or Win32
surface by default and does not reproduce all injected-module or guest-memory-map
behavior. Independent diagnostic switches can narrow those differences:

- `guest_placeholders: true` uses the emulator's `VirtualAlloc2` placeholder
  reservations for the guest intervals instead of ordinary `MEM_RESERVE`.
- `backing_mib: 16384` adds a lazily committed, writable 16 GiB section mapping
  with the emulator's `SEC_RESERVE` flags. The diagnostic maximum is 65536 MiB.
- `win32_surface: true` creates a hidden Win32 window and Vulkan surface before
  device enumeration. It does not create a swapchain or present frames.

All options default off. The backing mapping and hidden surface are destroyed
after the device. This remains a device-creation check, not a DLSS evaluation or
game rendering check.

Set `KYTY_DEVICE_CREATE_REPORT` to an absolute JSON path to record the exact
device creation inputs through the same helper used by the emulator. The report
includes extension names, every known core/chain feature value, queue priorities,
and a Windows address-region map (addresses and allocation metadata only, no memory
contents). Unknown chain types are explicitly marked. No report work runs when
the environment variable is unset. Exclude `address_space` when comparing device
parameters across processes.

The optional build targets `kyty_dlss_device_probe_fixedbase` and
`kyty_dlss_device_probe_highbase` compile the identical diagnostic with ASLR
disabled at image bases `0x140000000` and `0x100000000000`. They isolate executable
address-layout differences from Vulkan parameters; neither is part of the default
build or a change to the emulator's linker settings.

### RTX 5080 address-layout reproduction

On the RTX 5080 with driver 621.14, the same full device-creation configuration
succeeds with the default ASLR probe and fails with `VK_ERROR_INITIALIZATION_FAILED`
(`-3`) with both fixed-base probes. Core features, all 18 feature-chain structures,
extensions and queue creation inputs compare equal after removing `address_space`
from the reports. Merely moving a fixed executable from `0x140000000` to 16 TiB
does not fix this reproduction.

The smaller fixed-base comparison needs only these inputs:

```json
{"api_minor":3,"reservations":"guest","emulator_extensions":false}
```

It fails with the required NGX extensions. Changing only `reservations` to `none`
succeeds. Keeping the reservations and adding
`"omit_extension":"VK_NVX_binary_import"` also succeeds. Omitting a required
extension is a diagnostic comparison, not a usable DLSS configuration. Early CUDA
initialization reports `2` in both passing and failing cases, so its return value
alone does not identify this Vulkan failure.

Build the alternate executables explicitly and run them sequentially with the
same backend, runtime, JSON configuration and process environment:

```powershell
cmake --build C:/work/dlss-backend-build --target `
  kyty_dlss_device_probe kyty_dlss_device_probe_fixedbase kyty_dlss_device_probe_highbase
$env:DISABLE_RTSS_LAYER = '1'
$env:KYTY_DEVICE_CREATE_REPORT = 'C:/work/aslr-create-info.json'
C:/work/dlss-backend-build/kyty_dlss_device_probe.exe `
  C:/work/dlss-backend-build/kyty_dlss_backend.dll `
  C:/external/DLSS/lib/Windows_x86_64/rel `
  C:/work/device-config.json C:/work/aslr-result.json
```

Repeat with a distinct report/output path for each executable. The opt-in report
also records the loaded image base and virtual memory regions; disabling ASLR
changes more process allocations than the executable's image base alone. These
checks establish device creation only. The emulator subsequently passed its
SR-only graphics initialization and shutdown check with `/DYNAMICBASE
/HIGHENTROPYVA`. That check loads no guest code and does not establish DLSS
evaluation, gameplay stability, frame generation or visual quality.

## Host call order

1. Load the DLL; obtain `KytyDlssGetApi` with `GetProcAddress`. Initialize
   `KytyDlssApi.struct_size` and request `KYTY_DLSS_ABI_VERSION`.
2. Before creating a Vulkan instance, call `query_instance_extensions` and merge
   its names into the enabled instance extension list. Before creating a device,
   call `query_device_extensions` for the selected physical device and enable its
   returned names. These calls depend on the supplied runtime; do not assume the
   names are permanently fixed. On the tested RTX 3080/runtime the requirements
   were instance `VK_KHR_get_physical_device_properties2`, and device
   `VK_NVX_binary_import`, `VK_NVX_image_view_handle`,
   `VK_KHR_buffer_device_address`, `VK_KHR_push_descriptor`.
3. Enable `bufferDeviceAddress` and `shaderInt64` when creating the device; use a
   graphics+compute queue family. `create_context` receives the host's actual
   enabled extension list and feature attestations, dispatch functions, fixed
   input/output extents, quality mode and paths. It checks support and initializes
   NGX without creating a Vulkan instance, device, queue, command pool or fence.
   Version 1 permits one active context per process and requires serialized API
   calls. Uncoordinated NGX clients in the same process are unsupported. The
   experimental native-FG path shares the verified runtime directory and drains
   and frees FG resources before destroying SR, then shuts down Streamline.
   This controlled coexistence still requires live validation; the ABI does not
   provide general cross-client ownership guarantees.
4. `query_optimal` returns the runtime's recommendation for the requested output
   and quality. Context creation fixes extents; changing extents or mode requires
   host GPU completion and context recreation. Preset K is the default; an
   explicit `KYTY_GT7_DLSS_PRESET` requests K, M or L. The API does
   not claim that the runtime's effective model selection has been inspected.
5. Prepare actual pre-temporal HDR color, reversed depth and clean backward motion
   in **input render pixels**, with projection jitter removed from motion. Format
   requirements are RGBA16F, R32F and RG32F respectively; each is a one-mip,
   one-layer, single-sampled 2D color-aspect image covering exactly the input
   extent. The output is separate RGBA16F at the requested output extent. The host
   owns resource contents and must prevent aliasing, including aliased memory
   behind distinct image handles. ABI validation cannot inspect their pixels,
   image usage flags, views or memory allocations.
6. End dynamic rendering. In a recording primary command buffer on the selected
   family on the host's actual command-recording thread, synchronize input producers to shader reads and transition inputs to
   `SHADER_READ_ONLY_OPTIMAL`; synchronize prior output uses and transition output
   to `GENERAL`, with storage-image usage. Call `record_evaluate`. It records lazy
   feature creation once and evaluates the persistent feature. It neither begins
   nor ends command buffers, submits, waits, nor transfers queue ownership. All
   evaluations must execute in recording order on the same host queue. The first
   evaluation forces reset; later resets follow `reset_history`.
7. After recording, invalidate/rebind the host's cached pipeline, descriptors,
   push constants and dynamic state: NGX does not preserve Vulkan command state.
   Keep layouts tracked as input shader-read-only and output general. Add an
   appropriate shader-write-to-consumer barrier before reading/transferring the
   output. Keep all borrowed resources alive until host GPU completion. The next
   frame must not overwrite inputs or output while previous work still uses them;
   use the host scheduler's ordering/fences/timeline, without a per-frame idle.
8. At shutdown/recreation, wait in the **host** for all submitted work referencing
   the feature to complete (or discard commands that were never submitted), then
   call `destroy_context(context, 1, ...)`. Only then destroy the host device or
   unload the backend DLL. Passing zero is rejected; the flag is a host assertion,
   not a hidden backend wait.

`INVALID_ARGUMENT` from `record_evaluate` guarantees that this call recorded
nothing and did not poison the context. Every other non-OK recording status,
including internal exceptions and `POISONED`, requires abandoning the current
primary command buffer. Invalid ABI/format/dimension arguments are rejected
before recording. An NGX
creation/evaluation failure may leave partial commands in the command buffer and
marks the context poisoned. Discard that unsubmitted buffer; wait for any earlier
submitted feature work, then destroy/recreate the context. Do not submit a buffer
after a failed record call other than `INVALID_ARGUMENT`. If that buffer also
contains guest commands and the emulator cannot discard/recover it, terminate the
experimental session instead of silently continuing. Successful recording is not
a GPU-completion claim.

The fixed feature flags are HDR, reversed depth, low-resolution motion and auto
exposure. Motion scale is `(1,1)` after host conversion, MVJittered is disabled,
and no BiasCurrentColor mask is supplied. The host explicitly supplies
frame time, pre-exposure and exposure scale; auto exposure is not proof of matching
the game's exposure pipeline. Invalid or missing game motion still needs a
separate host fallback policy.

## Integration scope

The emulator already enables `bufferDeviceAddress` and `shaderInt64` in
`src/graphics/presentation/window/vulkanWindow.cpp`. Optional NGX extension
discovery must happen before its instance/device creation; a DLL loaded after
device creation cannot add the NVX extensions. Renderer input conversion, guarded
TAA matching, effect-layer replay, fallback compositing and scheduler integration
remain host work. This target changes none of those paths and does not add a
user-facing DLSS toggle.

## Native PSSR resource extension

`KytyDlssGetPssrApi` is a separate, optional versioned C export described by
`kyty_dlss_pssr.h`. It reuses this backend's existing signed runtime path and
`KYTY_GT7_DLSS_PRESET` policy. The original `KytyDlssGetApi` v1 layout, fixed GT7
resource contract and default behavior are unchanged.

A native session owns one NGX initialization on the host device. Individual
features have separate parameters and temporal history, and may have different
output dimensions. The session cannot be destroyed while any feature remains.
The native session and a v1 context are mutually exclusive. There is no second
driver NGX loader and no per-feature initialization/shutdown.

The SDK can return `NotImplemented` for the requirements query between shutdown
and a second initialization. The `--v1-reentry` control reproduced this in the
original v1 path. The backend now reuses an earlier successful requirements
result only for the same instance, physical device and runtime directory when
that exact error occurs. It still checks live DLSS availability and driver
requirements after every initialization; other errors remain failures.

Native evaluation accepts one selected mip/layer per borrowed image view,
including depth-aspect images, R11G11B10 HDR output, input subrect offsets, a
dynamic render extent within the queried mode range, native motion-vector
scales, jitter, explicit exposure or auto exposure, and history reset. The host
must supply truthful view/format/range metadata, layouts and barriers; it retains
all Vulkan resource ownership. This API performs super resolution only.

Feature allocation is CPU-only. The first evaluation records creation plus
evaluation. `INVALID_ARGUMENT` means validation recorded nothing and the caller
can use a spatial fallback. Every other recording failure requires abandoning
the command buffer; the emulator terminates rather than submit partially
recorded NGX commands. Deferred release must wait for all feature GPU work.

Run `kyty_dlss_backend_smoke <backend.dll> <runtime-directory> <output-directory>
--pssr` with absolute paths for the synthetic native-resource GPU test. It uses
no game files and exercises two concurrent histories, changing output sizes,
dynamic input size, depth aspect and mip/layer 1, input subrects, packed HDR
readback, exposure, reset, replacement and v1 lifecycle re-entry. A passing test
does not establish that GT7 actually issues native PSSR requests.
