# Native PSSR to DLSS SR

The native path forwards a supported game's Psml/PsmlMfsr2 temporal request through
the guest command stream to the external NVIDIA DLSS backend. **Tohi-GT7 1.0.4 uses
this path by default for 2560×1440 → 3840×2160 reconstruction, preset M, without
frame generation.** This consumes the game's native PSSR interface; it does
not execute Sony's PSSR neural model. It depends on the title declaring PS5 Pro
support and actually issuing native PSSR requests. Enabling the option alone does
not prove DLSS is running in GT7.

The 1.0.4 release supports **NVIDIA GeForce RTX 40 / 50 series GPUs only**. Its live
validation used **RTX 5080** and GT7 PPSA01317 01.710.000. Other GPU families are
outside the release's support scope; not every model in these two series was tested.

## Release 1.0.4 status

The 1440p SR-only test baseline underlying this release completed two November 26
driving runs with native 2560×1440 inputs and 3840×2160 DLSS output. These runs
used the earlier test candidate, not the final 1.0.4 artifact built with both FG
providers compiled out. Their last logged counters were respectively **38,400 / 38,400** and
**9,600 / 9,600** requests / DLSS evaluations, both with **zero linear fallbacks**.
All 67 and 19 bounded PSSR summary samples kept those dimensions. Both sessions
ended normally with exit code 0. The player reported that slow motion disappeared
and stutter was substantially reduced, with some minor stutter remaining.
These counters verify the sampled SR path, not display FPS, frame-time consistency,
latency, or image-quality acceptance for every car, track and view.

The package contains neither NVIDIA nor FSR frame-generation runtimes and adds no
30 FPS limit. Each presented game frame comes from an actual rendered frame after
reconstruction; this is not native 4K rendering. Fullscreen Mailbox presentation uses
the desktop's physical display size, independently of the 4K reconstruction target.
See the [release notes](RELEASE-1.0.4.zh-CN.md).

## Selecting the path in developer tools

The release launcher supplies this configuration. The options below are for source
builds and isolated developer tests, not additional steps required for normal play.

Build the emulator and `tools/dlss_backend` together. The optional backend must
export both the original `KytyDlssGetApi` and the new `KytyDlssGetPssrApi` entry
points. Keep the NVIDIA SDK and runtime external to the repository.

The release flags are `--gt7-desktop-height native --ps5-pro --pssr-dlss
--gt7-pssr-1440-4k`. Provide absolute paths through
`KYTY_GT7_DLSS_BACKEND` and `KYTY_GT7_DLSS_RUNTIME`, as with the existing path.
For native mode keep `KYTY_GT7_DLSS=0`, which disables the separate legacy TAA
hook; `--pssr-dlss` enables native SR. The release sets `KYTY_GT7_DLSS_FG=0`,
`KYTY_GT7_FSR_FG=0` and `KYTY_FG_MANUAL_ONLY=1`, clears inherited capture/FG
settings, and applies no extra base-frame limiter. Both frame-generation providers
are disabled at build time. Only `kyty_dlss_backend.dll` and `nvngx_dlss.dll` are
required for this SR path; Streamline, DLSS-G and Reflex DLLs are not required.
`KYTY_GT7_DLSS_PRESET=K`, `L`, or `M` retains the backend's existing model request.
The requested preset is logged; this does not independently verify which model
the NVIDIA runtime selects.

For the 1.0.4 Windows package, configure `KYTY_PACKAGE_DLSS=ON`,
`KYTY_ENABLE_GT7_DLSS_FG=OFF` and `KYTY_ENABLE_GT7_FSR_FG=OFF`. Use a clean install
directory: the package tools reject stale FG DLLs and licenses. CMake and the
package tools verify the pinned backend and SR runtime even when FG is disabled.

The developer script `scripts/GT7-Test-DLSS.ps1` accepts `-NativePssr` alongside
its existing required game, emulator, backend, runtime, save-source and output
paths. It uses the native internal-resolution profile rather than forcing the
legacy 1080p patch. Input and reconstruction dimensions come from each PSSR
request; the base `-NativePssr` mode's desktop window remains 2560 x 1440. The script
retains its isolated save-copy workflow.

Native mode rejects VR, FSR frame generation and legacy F9/F10 capture options.
For an isolated 1440p-to-4K test, add `-NativePssr1440To4K` without
`-NativeFrameGeneration`; diagnostics remain off unless explicitly requested.
Earlier opt-in FG development modes are documented in the
[historical NVIDIA integration notes](GT7-NVIDIA-FRAME-GENERATION.md). They are
not available in the 1.0.4 release build.

## GT7 Pro 1440p-to-4K release profile

For PPSA01317 01.710.000, add `--gt7-pssr-1440-4k` to `--ps5-pro --pssr-dlss`.
The dedicated `GT7-PSSR-1440-to-4K.json` profile changes the game's native scene
resolution controller. The native requests in both driving runs confirmed actual
2560 x 1440 color/depth/motion inputs with 3840 x 2160 output in the test baseline. The bridge preserves
their real resources, rectangles, jitter and motion scales; it does not relabel a
larger input as 1440p. Special viewports or unobserved scenes still require runtime
verification; a launch flag alone is not evidence of the input size.

The 1440p and historical 1080p fixed profiles are mutually exclusive. Profile
activation checks executable-image identity, profile digest and original patch
bytes; game files on disk are not rewritten. `--gt7-check-only` validates the
selection before GPU initialization or guest execution. The developer script
selects a 4K window for isolated testing; the release launcher presents fullscreen
at the desktop display size while keeping the 4K reconstruction target.

## Historical GT7 Pro 1080p-to-4K profile

For PPSA01317 01.710.000, add `--gt7-pssr-1080-4k` to `--ps5-pro --pssr-dlss`.
The playtest script selects it with `-NativePssr -NativePssr1080To4K`, requests a
3840 x 2160 window, and keeps frame generation off unless explicitly requested.
The original developer script's `-NativePssr` default is unchanged; the 1.0.3
launcher selected this dedicated fixed profile. That option rejects legacy
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

The following records describe November 25 candidates before the 1.0.3 fixed
1080p-to-4K frame-generation candidate. Their acceptance
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
