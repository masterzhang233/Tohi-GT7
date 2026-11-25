# Native PSSR to DLSS SR

The native path forwards a supported game's Psml/PsmlMfsr2 temporal request through
the guest command stream to the external NVIDIA DLSS backend. **Tohi-GT7 1.0.3 uses
this path by default for fixed 1920×1080 → 3840×2160 reconstruction, with NVIDIA
2X frame generation.** This consumes the game's native PSSR interface; it does
not execute Sony's PSSR neural model. It depends on the title declaring PS5 Pro
support and actually issuing native PSSR requests. Enabling the option alone does
not prove DLSS is running in GT7.

The 1.0.3 release supports **NVIDIA GeForce RTX 40 / 50 series GPUs only**. Its live
validation used **RTX 5080** and GT7 PPSA01317 01.710.000. Other GPU families are
outside the release's support scope; not every model in these two series was tested.

## Release 1.0.3 status

The November 25 candidate completed a real Nürburgring driving run with native
1920×1080 inputs, 3840×2160 DLSS output and NVIDIA frame-generation activity. The
player reported that interruptions were substantially reduced or absent during
the requested comparison. The run ended normally. Camera/input and HUD checks
remain active; unverified scenes can still suspend frame generation. These results
do not establish continuous 2X, a display-FPS gain, or image-quality acceptance for
every car, track and view. See the [release notes](RELEASE-1.0.3.zh-CN.md) and
[frame-generation contract](GT7-NVIDIA-FRAME-GENERATION.md).

## Selecting the path in developer tools

The release launcher supplies this configuration. The options below are for source
builds and isolated developer tests, not additional steps required for normal play.

Build the emulator and `tools/dlss_backend` together. The optional backend must
export both the original `KytyDlssGetApi` and the new `KytyDlssGetPssrApi` entry
points. Keep the NVIDIA SDK and runtime external to the repository.

The emulator flags are `--ps5-pro --pssr-dlss`. Provide absolute paths through
`KYTY_GT7_DLSS_BACKEND` and `KYTY_GT7_DLSS_RUNTIME`, as with the existing path.
For native mode keep `KYTY_GT7_DLSS=0`, which disables the separate legacy TAA
hook; `--pssr-dlss` enables native SR. When NVIDIA frame generation is enabled,
`KYTY_GT7_DLSS_RUNTIME` and `KYTY_GT7_DLSS_FG_RUNTIME` must resolve to the **same
directory**, containing the SR and Streamline/DLSS-G runtime DLLs supplied together.
Separate SR and FG SDK directories are not the supported release layout. The
launcher configures and checks this automatically; the complete native developer
example is in [the frame-generation notes](GT7-NVIDIA-FRAME-GENERATION.md#build-and-activation).
`KYTY_GT7_DLSS_PRESET=K`, `L`, or `M` retains the backend's existing model request.
The requested preset is logged; this does not independently verify which model
the NVIDIA runtime selects.

The developer script `scripts/GT7-Test-DLSS.ps1` accepts `-NativePssr` alongside
its existing required game, emulator, backend, runtime, save-source and output
paths. It uses the native internal-resolution profile rather than forcing the
legacy 1080p patch. Input and reconstruction dimensions come from each PSSR
request; the base `-NativePssr` mode's desktop window remains 2560 x 1440. The script
retains its isolated save-copy workflow.

Native mode rejects VR, FSR frame generation and legacy F9/F10 capture options.
In the developer script, NVIDIA frame generation is opt-in with
`-NativeFrameGeneration` and an explicit `-StreamlineRuntimeDirectory`. The native
bridge supports the verified full-frame
1920 x 1080 to 3840 x 2160 path and requests exactly one generated frame per real
frame (fixed 2X). Other dimensions or unverified scenes retain ordinary presentation.
The release launcher explicitly selects the native 1080p-to-4K and frame-generation
options; inherited FG settings alone are not a substitute for the required inputs.

The bridge copies the actual native depth and backward motion inputs after a
successful DLSS evaluation, applying the request's pixel scale once. It matches
a verified producer's camera and projection jitter to those exact image identities,
then freezes the 4K display color after the verified tone-map and before HUD draws.
Writes to inputs still needed by the current scene, multiple requests, missing camera
data or a mismatched flip invalidate the candidate. Once verified tone-map and
HUD-less capture complete, legitimate reuse of the retired SR source no longer
invalidates the frozen display image. The native path does not execute the legacy
TAA-hook SR feature. CPU tests and compilation alone do not prove live FG activity
or performance; runtime presentation counters and user observation provide separate
evidence, and neither is a display-FPS benchmark by itself.

## GT7 Pro 1080p-to-4K profile

For PPSA01317 01.710.000, add `--gt7-pssr-1080-4k` to `--ps5-pro --pssr-dlss`.
The playtest script selects it with `-NativePssr -NativePssr1080To4K`, requests a
3840 x 2160 window, and keeps frame generation off unless explicitly requested.
The original developer script's `-NativePssr` default is unchanged; the release
launcher selects the dedicated fixed profile. That option rejects legacy
desktop-height patches, VR and custom patches. `--gt7-check-only` validates it before GPU initialization
or guest execution. Executable-image identity, profile digest and original patch
bytes are checked by the existing profile loader; no game file is rewritten.

`GT7-PSSR-1080-to-4K.json` fixes the native scene controller endpoints at 0.5.
For the standard 3840 x 2160 Pro viewport, the game's 128-pixel quantizer retains
0.5 and its target sizing produces 1920 x 1080. Offline execution of the extracted
controller, Pro quantizer and target-sizing instructions covered 686 combinations.
The native target setter also updates its descriptor and viewport together.
Special viewports, per-scene multipliers, secondary targets and scene bypasses
may differ. The static investigation has not independently identified every
color/depth/motion producer; in-game native requests remain the acceptance evidence.

The bridge keeps the game's actual rectangles, resources, jitter and motion
scales. It never relabels a higher-resolution image as 1080p. A larger allocated
backing is valid when the actual render viewport is smaller. This uses the native
PSSR entry point with NVIDIA DLSS reconstruction, not Sony's PSSR neural model.

The opt-in `shader_recompiler_compute_tests --pssr-dlss-1080-4k-integration`
fixture exercises the real PM4/texture-cache/renderer/NGX path with independently
allocated 1920 x 1080 color, depth and motion, and a 3840 x 2160 output. It checks
eight evaluations, no spatial fallback, temporal jitter, low-resolution motion,
history/reset behavior and every output pixel for finite values. This synthetic
fixture cannot establish that a game scene uses those dimensions or looks correct.
`-NativePssrDiagnostics` records the original requests and matched output samples
for later verification, without F9/F10; sampling adds overhead.

`scripts/Measure-GT7NativePssrResolution.ps1 -RunDirectory <run>` summarizes the
actual logged input/output pairs and other dimensions. It does not infer success
from launch flags, require allocation size to equal the active rectangle, or
treat a bounded diagnostic stream as complete track coverage. Its optional
`-ReportPath` must be outside the original run directory.

## Data and resource contracts

Psml exports validate the descriptors and emit a 208-byte request. The renderer
resolves the original Vulkan images and selected mip/layer, performs barriers,
then records DLSS into the guest output. It supports dynamic input rectangles,
native motion-vector scales, exposure, reset and reversed-depth flags. Each
library context lifetime gets its own history identifier; multiple histories
share one NGX initialization. Replacing a history defers destruction until its
GPU work has completed.

The current guest output contract is RGB R11G11B10 float, standard 256-byte
tiling, one base mip and one array layer. Unsupported guest output descriptors
are rejected before queuing work. Missing or unsupported temporal inputs use a
spatial bilinear fallback with the correct input rectangle. This fallback is
not DLSS or a PSSR neural reconstruction. Re-entering DLSS after a fallback
resets temporal history.

The new optional backend ABI preserves the old v1 ABI. The backend records into
host-owned command buffers and does not submit or wait. Invalid inputs are
rejected before recording; an NGX failure after recording may have begun is
fatal rather than submitting a partly recorded buffer with fallback appended.

## Verifying execution

- `[PSSR guest]` records native guest dispatches.
- `PSSR DLSS:` evaluation counters record successful native DLSS evaluations.
- `PSSR linear fallback:` and `PSSR summary:` distinguish spatial fallback from
  successful DLSS work. Backend readiness or a Pro-mode message is insufficient.

Targeted tests cover Psml validation/packet generation, Pro capability gating,
expanded memory bounds, temporal quality selection and the Vulkan PSSR renderer.
The standalone backend smoke accepts `--pssr` for synthetic native resources;
the original `--quality-1440p` smoke checks the existing GT7 ABI. These do not
validate GT7's Pro branch, in-game image quality, frame rate or frame generation.

## Historical SR-only image-quality investigation (2026-11-25)

The following records describe earlier candidates from the same day, before the
fixed 1080p-to-4K frame-generation candidate summarized above. Their acceptance
status and dynamic-resolution settings apply to those candidates, not the current
release preset.

The first GT7 playtest recorded at least 3600 native DLSS evaluations, including
2560 x 1440 input reconstructed to 3840 x 2160, with zero linear fallbacks at the
last logged counter. The user reported intermittent white-screen flashes while
driving. Image-quality acceptance therefore failed for that candidate, which was
not ready to replace the established GT7 preset. Frame generation was disabled. The process
returned zero after the user closed it, but its log also contains trailing host
access violations, so zero alone does not establish a clean shutdown.

A separate 38-frame RTX 5080 backend test did not reproduce a brightness blowout
with valid synthetic input. It covers RGBA16F and R11G11B10 color, dynamic input
sizes, nonzero subrects, two histories, reset, and changing pre-exposure and
exposure texture values. It waits for a fence after each evaluation and does not
validate the emulator's live resource contents or cross-submission behavior.
This is isolation evidence, not a white-flash fix.

The opt-in `shader_recompiler_compute_tests --pssr-dlss-integration` test also
passes through the actual PM4, texture cache, renderer and native NGX backend.
Set absolute `KYTY_GT7_DLSS_BACKEND` and `KYTY_GT7_DLSS_RUNTIME` paths first.
It runs 59 real evaluations with no fallback: 16 combinations of dynamic input
size, CPU input refresh, exposure and pre-exposure, three evaluations in one
command buffer without intervening host readback, then 40 evaluations that
change the actual color/depth/motion backing sizes at the same guest addresses.
The backing-size phase retains the native history and holds each size for eight
evaluations, advancing actual presented-frame and scheduler counters. At least
one old color alias must be retired for this coverage to pass. Running with
diagnostics off and on produced the same 57 measured output errors. All 59
exposure samples matched the fixture values. This uses CPU-produced synthetic
color/depth/motion resources, starts after the guest Psml ABI, and does not
cover GPU producers, compressed depth metadata, or the game's white flashes.

A subsequent diagnostic playtest captured 1828 matched input/output sample
pairs, with zero fallbacks or sampled nonfinite values. The user confirmed brief
white flashes that immediately recovered. Several later one/two-request
brightness spikes were already present in the 4x4 input samples: for example,
input mean luminance changed from 0.379 to 1.465 and back to 0.377, while DLSS
output followed it. These are candidate source-image flashes, not a proven
match to the displayed symptom: there is no full-frame or final-presentation
capture or user timestamp. The initially selected examples occurred four/five
requests after active-size changes; the complete same-threshold scan found ten
late short pulses, two to five requests after a change. The synthetic backing-size
test above did not reproduce a spike.
The exact live input-production/cache cause remains unresolved. Changing
exposure or suppressing bright frames is not justified by these measurements.

The investigation reproduced a separate shared-HTile lifetime defect: deleting
one depth image erased clear/fill state still used by another depth binding.
HTile state now tracks its actual bound image owners, survives individual owner
retirement, and is removed after the last owner leaves. Retargeting a binding
and explicitly unmapping any byte of its metadata allocation update ownership.
The isolated `--shared-htile-lifecycle-only` regression failed before this change
and passes afterward, including consumed-clear, known-fill, retargeting and
unmap/re-registration cases. The Psml/PSSR contract tests and 59-evaluation
native integration also pass. The broader existing `--htile-clear-only` test
still reports `ErrorDeviceLost` at tick 14 both before and after the change, so
broad texture regression coverage is incomplete. This is a candidate cache
fix; no game run has yet established that it removes the reported flash.

The user's subsequent HTile-candidate run ended with exit code 321 in the
draw-preparation worker: ordinary vertex preparation called the strict draw-time
buffer grouping routine, which terminates on unequal record counts in one
interleaved binding. Unlike tessellation, the ordinary vertex scan had not been
routed through clean-backing snapshots and a nonfatal rejection path. It now
uses registered metadata and bounded local table copies, declines mismatches
without publishing partial outputs, and leaves the real draw to its normal
GPU-thread path. Merged mesh stages also decline speculative preparation.
The real draw's buffer-range contract is retained; record counts are neither
expanded nor truncated to hide the mismatch. The captured log does not contain
the original descriptors, so it does not prove whether the mismatch was a
transient update or a persistent unsupported layout.

`shader_cfg_tests --vertex-prep-safety-only` covers real guest-backed tables:
successful scan, mismatched record counts, recovery, independent binding counts,
and unreadable pointers, plus shader registration, SGPR limits and wave sizes.
It runs without the game or a GPU render. The failed candidate run had 1357
matched native input/output samples and no PSSR fallback/failure; its shorter
late segment did not contain the earlier sampled brightness pulses. That is
not image-quality acceptance or proof of a white-flash fix.

The following user run of the vertex-preparation candidate completed normally
after about 4.5 minutes. It recorded 7372 matched native DLSS input/output sample
pairs, no logged fallback or sampled nonfinite values, and no fatal/access-violation
messages. The user explicitly reported no further white flash. Its input remained
dynamic, 2560 x 1440 through 3840 x 2160; it did not test the separate 1080p-to-4K
candidate. This supports the tested version as the preserved comparison build,
without establishing the earlier flash's root cause or general stability.

The playtest script's opt-in `-NativePssrDiagnostics` switch requires `-NativePssr`.
It enables bounded automatic GPU input/output and exposure sampling without the
legacy F9/F10 capture path. Diagnostic overhead means the run is not a performance
benchmark. Guest request rejections are also logged with bounded repetition even
in Silent mode; success counters alone cannot reveal a rejected guest request.
