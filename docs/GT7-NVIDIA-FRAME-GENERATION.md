# GT7 DLSS SR and native Vulkan frame generation

> **Historical 1.0.3 and development documentation.** Tohi-GT7 1.0.4 uses
> [native 1440p-to-4K DLSS SR](NATIVE-PSSR-DLSS.md), without NVIDIA or FSR frame
> generation, their runtime DLLs, or the experimental 30 FPS cap. The activation
> instructions below do not apply to the 1.0.4 binary package.

**Tohi-GT7 1.0.3 defaults to native 1920×1080 → 3840×2160 DLSS reconstruction and
NVIDIA 2X frame generation. The release supports only NVIDIA GeForce RTX 40 / 50
series GPUs; live validation used an RTX 5080.** Other GPU families are outside
this release's support scope. Not every supported model has been tested.

The native PSSR bridge consumes the game's temporal request and reconstructs it
with the PC DLSS backend, not Sony's PSSR neural model. Developer tools select it with
`-NativePssr -NativePssr1080To4K -NativeFrameGeneration`. It uses the native
request's depth, motion scale and jitter plus the independently verified camera
producer and 4K HUD-less tone-map. It never invokes legacy SR in native mode.
The bridge requests fixed 2X (`numFramesToGenerate=1`), checks SDK support before
enabling, and suspends FG on invalid inputs or runtime state failures. No dynamic
or higher multiplier is requested. **2X means one generated frame per original
frame; this release does not offer 3X, 4X or dynamic MFG.** Preset M is an SR model
request and is unrelated to the number of generated frames. The legacy 1080p-to-1440p
TAA-hook path remains available for development; it is not the 1.0.3 launcher default.
Both paths preserve the original game/timer/input cadence and require real camera,
depth, motion and HUD-less inputs. Missing evidence never becomes a zero buffer
or a guessed camera.

## Release 1.0.3 validation and limits

The November 25 RTX 5080 test recorded native 1080p-to-4K SR and additional NVIDIA
presentations during Nürburgring driving, then exited normally. The player reported
that interruptions were substantially reduced or absent in the requested comparison.
Camera-pair validation and scene-resource lifetime fixes address interruptions found
in earlier candidates. HUD-less rejection and unverified scene transitions can
still suspend generation, so this is not a claim of uninterrupted 2X for every scene.

The candidate passed 21 focused test groups. The full suite passed 104 of 109 tests;
the remaining five reproduced the same failure signatures as the pre-change baseline.
These tests do not replace visual acceptance. The final driving run was not a
PresentMon measurement or a display-FPS benchmark. The **game's FPS counter counts
original game frames**, not generated presentations, and SDK counters alone do not
measure unique displayed content or prove a constant multiplier. See the
[1.0.3 release notes](RELEASE-1.0.3.zh-CN.md).

## Build and activation

Build with `KYTY_ENABLE_GT7_DLSS_FG=ON` and point
`KYTY_GT7_STREAMLINE_SDK` at an externally installed NVIDIA Streamline 2.14.16
SDK. SDK headers and runtime binaries are not part of this source tree. The
external NGX Super Resolution backend remains a separate DLL.

For a direct **native 1080p-to-4K** launch, clear inherited experimental `KYTY_*`
and `GT7_*` overrides as the launcher does, then set these on that child process.
Use the **same resolved runtime directory** for SR and Streamline FG, containing
`nvngx_dlss.dll` and the Streamline/DLSS-G DLLs shipped together in the release.
Do not point the two variables at separate SDK directories.

```
KYTY_GT7_DLSS=0
KYTY_GT7_DLSS_BACKEND=<absolute kyty_dlss_backend.dll>
KYTY_GT7_DLSS_RUNTIME=<absolute shared runtime directory>
KYTY_GT7_DLSS_PRESET=M
KYTY_GT7_DLSS_FG=1
KYTY_GT7_DLSS_FG_RUNTIME=<the same absolute shared runtime directory>
KYTY_GT7_FSR_FG=0
KYTY_FG_MANUAL_ONLY=1
KYTY_GT7_NATIVE_ASYNC_PRESENT=0
DISABLE_RTSS_LAYER=1
```

Supply the native flags with the usual game arguments:

```
--gt7-desktop-height native --ps5-pro --pssr-dlss --gt7-pssr-1080-4k
```

`KYTY_GT7_DLSS=0` disables the **legacy TAA hook**; the native `--pssr-dlss` flag
enables the SR path used here. A legacy `KYTY_GT7_DLSS=1` development example is
not equivalent to the release configuration. Normal package users should use the
launcher, which checks all nine required runtime DLLs before starting.

`scripts/GT7-Test-DLSS.ps1` also accepts `-NativeFrameGeneration`,
`-StreamlineRuntimeDirectory`, and `-DlssPreset M`, with the same independent
save-copy and graphics-only checks as the existing playtest. Native and FSR
frame generation are mutually exclusive. The FSR occlusion correction and F10
FSR output-capture helper do not run on the NVIDIA path.

Preset M requests the DLSS 4.5 model. The legacy path's custom 75% input ratio may
select a model hint associated with a different standard quality ratio, so an
explicit preset sets every official ratio-specific hint. This example describes
the legacy developer path, not the release's fixed 1080p-to-4K ratio. Leaving the
preset unset preserves the previous K configuration. The backend records requested presets separately from
effective runtime behavior; `-DiagnosticLogging` enables the original NGX model
selection messages for inspection.

## Vulkan and lifetime contract

Streamline initializes before Vulkan instance creation. Its interposer supplies
the dispatch table and adds required extensions, features and private queues.
The SDL Win32 surface is also created through that dispatch, registering its
HWND for Streamline's swapchain. The host supplies the missing `VK_KHR_present_id`
dependency of Streamline 2.14.16's Reflex `VK_NV_low_latency2` request, and enables
the present-ID feature when supported.
The Vulkan 1.3 private-data feature is enabled when available because the
interposer creates a private-data slot.
It owns the presentation hooks; no D3D12 bridge and no nested FSR swapchain are
involved. The implementation requests exactly one generated frame (2X).
It does not implement dynamic multi-frame generation.

Display color and HUD-less color use the same output format, size and conversion.
Depth and motion retain the real 1080p input extent. Motion contains clean
current-to-previous displacement in pixels and is normalized to texture UV units
for Streamline. Camera reprojection derives from the verified right-handed view
and reversed-depth projection. TAA jitter is supplied separately.

The present command buffer tags depth, motion and HUD-less images as
`eOnlyValidNow`, allowing Streamline to record private copies before frame-pool
reuse. Recording invalidates the present scheduler's cached Vulkan state.
Missing inputs, overlays, skipped frames and discontinuities suspend generation
and reset history on resumption. Present bookkeeping is outside the application's
queue mutex to preserve drain lock ordering.
The presenter creates its frame token and performs Reflex pacing after successful
swapchain acquisition, before taking the shared renderer mutex. The SDK's CPU
sleep must not hold that mutex and block guest command recording. Preparation and
present markers use the same token; acquire failures do not create one, and
swapchain retries begin a new frame.
Reflex options are reapplied after each successful swapchain creation. In the
2.14.16 Vulkan low-latency2 path, setting them before a swapchain exists returns
success without configuring that swapchain; its creation hook does not replay
the options. The application now checks the post-create call and suspends FG
on failure. This does not add a frame-rate cap or move the existing sleep call.

Long-term drain stops generation, waits through the interposed device-idle hook,
and frees DLSS-G resources and cached views before frame input images are freed.
The SR context is then destroyed before Streamline shutdown. The interposer DLL
remains loaded through Vulkan device and instance destruction.

## Historical development evidence (November 8–13)

The records below describe the earlier legacy TAA-hook experiments and their
individual acceptance status. They preserve the investigation history; the current
native 1080p-to-4K release status is summarized above.

The Windows clang-cl executable enables 64-bit ASLR. On this driver the former
`/DYNAMICBASE:NO` layout plus guest address reservations caused
`VK_NVX_binary_import` device creation to fail before any game ran. Matching
device probes reproduced the failure at both low and high fixed image bases;
the same Vulkan inputs passed with ASLR. Guest mappings remain fixed, and host
symbols use the actual module base. The test script opts out of RTSS's implicit
Vulkan layer for the child process only, following a crash through that layer
during the initial checks; it does not change global RTSS configuration.

Normal validation uses the Khronos layer without requesting unused
`VK_EXT_validation_features` overrides, which the Streamline instance proxy
otherwise rejects as a layer-only extension.

The development machine reports RTX 5080, driver 621.14 and hardware GPU scheduling
enabled. The official runtime's eligibility probe accepted DLSS-G. Standalone
SR tests accepted four synthetic frames for K/M/L, and four captured GT7 frames
with M passed input-conversion comparisons and finite-output checks. These checks
do not establish live race frame generation, visual quality or monitor FPS.

On 2026-11-08 the second Nürburgring playtest recorded a sustained interval of
186 application presents and 372 SDK-reported presentations, with a matching
186-frame increase in the conservative generated count. It exited normally
with code 0. The SR runtime logged the application's M hint for all six ratios.
This establishes real native 2x activity in that run, not continuous track
coverage, measured display FPS, or a public effective-model query. The player
reported acceptable interpolation appearance but intermittent generation and
frequent severe stalls, with better coverage outside the cockpit. Acceptance
therefore remains incomplete.

The follow-up fixes address two independently observed defects. A verified
geometric scene effect was rejected whenever the guest AA register retained
four-sample values, even though the actual color target and Vulkan pipeline
were single-sample. Replay now checks actual attachment and sample-dependent
behavior; partial coverage, real MSAA, depth, sample shading and shader coverage
exports remain rejected. This guard also affects the existing FSR input path.
Separately, test launches always copied the release seed's old shader cache.
Both native tests logged zero source hits and 4,095 misses with the same
recompiler version. Launches now copy the latest successfully completed run's
nonempty GT7 cache independently of career-save selection. Driver pipeline
caches are selected independently as well: the first native race retained about
190 MiB of driver cache, whereas the second had only the shader cache. Copying
only `_ShaderCache` would still lose that driver work. The engine validates GPU,
driver, UUID and payload integrity before accepting driver cache data. These
copies and fixes do not establish that every stall is gone.
Reflex pacing was also moved outside the renderer lock. Five-second CPU timing
summaries cover sleep, prepare, present, present intervals, mode changes, tagging
and state queries, including counts over 100 ms. These are diagnostic wall times,
not GPU timings or proof that a particular stall was caused by Streamline.

Native status reads `slDLSSGGetState` once after each application present. The
SDK reports presented frames since the preceding query, not a monotonic total.
Logs retain application presents, accumulated SDK reports and a conservative
excess count. Enabled settings and loaded DLLs alone never mean active generation.
The title's game FPS continues to count original game frames.
Periodic shared-input coverage totals distinguish accepted, scene, camera,
HUD-less, duplicate and unavailable snapshots without enabling full API traces.
Unknown UI writers receive bounded pixel-resource metadata logging without
changing acceptance or initiating an automatic frame capture. Multiple scene
chains in one guest frame and unverified UI layers still fall back until their
relationship to the final presented image has been established.

During that earlier experimental acceptance, the launcher release preset remained
unchanged. The target was fixed 2X; track/camera coverage still depends on the shared
verified input path. HUD-less input assists the runtime's UI handling but is not
a separately extracted alpha UI plane. Race testing must still check cockpit
views, Monza, Nürburgring, camera cuts, HUD stability and wheel ghosting.

## 2026-11-08 follow-up: stutters and wheel/rear artifacts

The `141621` Nürburgring run reused the shader and 190 MiB driver caches but the
player still reported several stalls plus wheel/rear ghosting and flicker. The
same executable/runtime/preset in the `142708` SR-only A/B produced no reported
ghosting/flicker, while stutters remained. This separates an FG-related visual
issue from a stall that also occurs without either FG implementation. No motion
scale, jitter, camera, or scene-effect acceptance change was justified by this
comparison alone.

The diagnostic-only `180712` run exited normally. During about four minutes of
driving it recorded ten 315–433 ms gaps before `Presenter::Impl::Present`, whose
own work took 0.75–16.31 ms in those cases. There was no coincident long native SDK
call, host GPU wait or synchronous pipeline compilation in the recorded data.
Cache counters showed 2246 source hits / 97 misses and 2686 permutation hits / 161
misses, with no rejected entries. The player still reported many stalls and
less noticeable artifacts; the latter is not attributed to a rendering fix.

`GT7-Test-DLSS.ps1 -PerformanceDiagnostics` keeps the per-API trace off and adds
bounded CPU stage, GPU wait, shader-cache and pipeline-build summaries to stdout.
All use steady-clock timestamps for correlation. Presenter prelude and video-out
queue diagnostics distinguish time waiting for a new game frame from time spent
presenting it. `-SampleGpuThread` additionally samples only `Thread_Gpu` at 100 Hz
with one-second reports; it requires performance diagnostics and is intended for
a short investigation, since sampling itself can perturb timing. Neither option
changes interpolation inputs, draw acceptance, synchronization or cache format.
Visual acceptance and the exact upstream stall cause remain unresolved.

The subsequent `184325` run exposed a separate presentation stall before any
verified race/FG interval: repeated `title` stages took approximately 3000 ms
while the flip-queue head was already ready. `UpdateTitle` synchronously waits
for an SDL main-thread callback, and `Flip` holds the port mutex across that
wait. Thus a window-title update can also block new flip reservations. These
events are distinct from the earlier race gaps, whose title stages were short;
fixing title delivery alone does not establish that race stutter is resolved.
Title updates now publish the latest text through one pending SDL user event,
at most four times per second. The main event loop consumes the text without
holding its mailbox mutex while changing the title. The event carries only a
window ID, so it cannot retain stack pointers or an asynchronous callback into a
destroyed context. Game-FPS accounting still runs on every original present.

The completed `184325` race reproduced eight 315–466 ms gaps. During each gap
the video-out thread continued checking an empty flip queue; reservation and
preparation of the next frame happened near the end of the gap. Each associated
one-second GPU-thread sample batch contained `RenderContext::UnmapMemory` →
`CommandScheduler::Finish` → `MasterSemaphore::Wait`, accounting for roughly
110–220 ms of sampled time per batch. This may represent many short waits,
not one long semaphore wait. Shader compilation, presenter locks, and the title
stage do not explain these race intervals.

The next candidate skips an unrelated GPU drain for CPU-only unmaps, following
upstream `e6fce45d3afd399aba245c3f5be06c47be7cfeec`. The decision stays on the GPU
command lane and requires no registered buffer, dirty/pending readback bytes,
overlapping live image, or ordinary/priority completion callback. The normal
invalidation and mapping cleanup remain ordered before the host unmap. Setting
`KYTY_UNMAP_SKIP_UNUSED=0` restores the previous drain behavior. The targeted
`gpu_command_lane` and `command_scheduler_timeline` tests passed; a graphics
startup check exited 0 and confirmed post-swapchain Reflex setup. These checks
do not establish smooth gameplay.

Candidate `efbc5309` in the completed `210223` run exited 0. After its first
reported generated frames, 25,352 of 26,572 reported unmaps skipped the drain
(95.4%), and no individual reported unmap exceeded 6.173 ms. Native generated
count increased by 10,071. Nevertheless, ten intervals of 116.786–966.328 ms
remained. Eight had matching flip-queue reports, all waiting for an unreserved
next game frame. The recorded unmap, presenter and pipeline timing cannot
explain those full gaps. The player still reported frequent stalls; acceptance
remains failed. Kernel memory-operation locking, batched guest memory operations
and host backing changes lie outside the renderer-unmap measurements and need
separate evidence before attributing the remaining stalls to them.

The same run also exposed a separate continuity issue. In approximately 230
seconds of race input coverage, scene rejection increased by 560 while HUD
rejection increased by one and camera-only rejection did not increase. There
were 265 enable/disable cycles and 276 history resets. Rejection samples show
two temporal evaluations/composites and late camera writers within one captured
boundary. Thirty-four five-second windows switched FG without a >100 ms gap.
This is evidence of intermittent input coverage, not proof that resets caused
the long stalls or that accepting multiple temporal passes would be correct.
Frame-to-scene ownership needs to be established before relaxing any guard.

The subsequent diagnostic candidate records kernel unmap and batch-map stages,
including the outer batch lock and per-operation totals. It formats reports only
after the existing outer memory lock has been released; it does not change
mapping operations. FG association histograms and bounded before/failure/after
metadata distinguish multiple temporal scenes from split producer boundaries.
Video-out preparation records identify their request, bus, source and image
range. Match their steady-clock microsecond interval to `snapshot_epoch_us`
(or `boundary_epoch_us` for blank frames), rather than assuming log-line order
is a shared frame index. Detailed preparation records stop after 20,000 entries
plus 128 exceptional entries, with an explicit exhaustion marker and ongoing
dropped counts. All these diagnostics require `KYTY_GT7_PERF_DIAGNOSTICS=1`;
they add no GPU captures and are not a further stutter fix. The diagnostic build
passed `virtual_memory_allocation` and `gpu_command_lane` with diagnostics
enabled, and compatibility preflight. No race acceptance is implied.

The `235653` diagnostic run confirms one source of that continuity failure.
For example, snapshot epoch `97302151165` us falls inside MAIN CPU flip request
6546's preparation interval `[97302151150, 97302151211]`. That request sees two
successful temporal passes: the same depth/MV and scene targets, consecutive
history ping-pong images, and different jitter. Request 6547 immediately follows
with zero temporal passes or composites; request 6548 has one again. Three
further bounded incidents repeat this 2/0/1 pattern with exact request matches.
All of the first 20,000 detailed preparation records are MAIN CPU flips.
The CPU flip queue can therefore deliver an older output after recording of
the next scene has begun. A presentation boundary is not a safe place to clear
the one global scene capture. Accepting the last TAA or reusing old inputs would
not fix the ownership problem.

The completed `235653` run exited 0 after the player reproduced stutters. After
the first positive generation report, it recorded seven 133–316 ms source-frame
gaps; the initial roughly 39-second interval without a 100 ms gap was not the
whole run. The SDK excess count reached 6,745, with 133 on/off cycles and 144
history resets. Later HUD rejections do not establish that the player was paused.
The first 20,000 detailed preparation records cover only a bounded portion of
the run; exhaustion of that budget is not evidence that later preparation was
absent. Presenter/native API timings remained short in the reported long gaps.

### 2026-11-09 candidate: completed outputs and mapping lookup

Verified scenes now retain their own depth, motion vectors, HUD-less color and
camera metadata in at most four reusable GPU slots. An actual presented output
must match its complete image identity, generation, backing and guest range.
A normal CPU flip does not reset the next live scene. The next verified camera
producer starts a new scene after the previous tone-map has completed; a
rejected unfinished scene still needs a real flip before recovery. Unknown
writes invalidate affected candidates, and unbounded writes invalidate all.
The single-TAA/composite/tone-map and UI acceptance rules remain enforced.
There is no fallback to the last mutable input set. Consumption copies into the
existing presenter frame's private images on the ordered GPU lane before a
candidate slot may be reused. The pool uses up to about 222 MiB at the current
input/output sizes, allocated on demand.

An atomic per-image external-write revision survives CPU/buffer invalidation
and refresh completion, including a snapshot taken after invalidation but before
upload. Ordinary verified GPU/UI writes do not advance it. Image identity must
also match. Separate CPU writes to DCC metadata are not covered by this pixel
revision; existing compute-write guards still require expanded-DCC proof.
Metadata-triggered conditional clears cannot be marked as actual pixel changes
on every lookup, since the usual race lookup is a no-op. A persistent metadata
CPU-write revision remains an explicit review boundary, not a verified cause
of this run's stalls or a claim of complete metadata coverage.

The long producer stalls also coincide with resource map/unmap bursts. The
largest later gap's surrounding five-second batch report contains 2,438
operations taking 292 ms in total, including 165 ms in `UnmapMemoryRange`.
That timer includes host mapping, mapping bookkeeping and range cleanup, not
only Windows syscalls; the aggregate does not assign every operation to one
gap. CPU samples independently locate `PhysicalMemory::Unmap` linear scans and
vector compaction in these bursts. Isolated placeholder coalescing measurements
were only a few milliseconds and did not justify changing host transactions.

Physical mappings are now indexed by virtual address in an ordered map. Exact
lookup, prefix removal, splits and protection updates avoid repeated full-table
scans and vector movement. The original mapping locks, single-mapping unmap
semantics, host placeholder transactions, physical alias ownership and failure
rollback are preserved. The caller also stops requesting an unused host address
that previously required another full scan.

CPU-only A/B measurements use 640 direct 64 KiB map/unmap operations per round,
four rounds per background mapping count. Total medians are:

| Background mappings | Previous vector | Address index |
| --- | ---: | ---: |
| 0 | 4.546 ms | 3.691 ms |
| 4,096 | 7.951 ms | 3.925 ms |
| 16,384 | 27.824 ms | 4.196 ms |

The largest group improves by 84.9% (ranges 27.195–31.623 ms versus
3.999–4.666 ms). These are real kernel mapping operations with no GPU work,
not a game FPS measurement. Automatic-memory reclaim alias scans and actual
driving are outside this benchmark. Full virtual-memory tests, including small
maps, alias survival, splits and injected rollback failures, pass. Candidate
association tests also pass. The candidate additionally passes ten selected
tests covering camera/association/effect/DCC/status, GPU command-lane image
revision, scheduler timeline, metadata classification and output write guards.
The installed executable matches the build, and compatibility/real Vulkan
startup checks exit 0. No validation layer was requested for that startup.
Race run `20261109-010742-4b4367b8` uses the previous run's save/cache and omits
thread sampling. It exited 0, but player acceptance failed: the player still
reports pronounced uneven motion and stutters on a directly connected monitor.
In a 160.314-second continuously generating interval, application and generated
counts both increased by 7,185, without on/off transitions. The 7,185 adjacent
MAIN source intervals have P50/P90/P95/P99 of 21.202/29.559/32.769/55.618 ms;
99 reach 50 ms, four reach 100 ms, and the maximum is 124.859 ms. Prepare itself
has P99 0.191 ms and maximum 0.361 ms. Five fully logged long flip gaps wait for
an unreserved next request rather than an already queued frame or a long native
Prepare/Present call. The completed-output fix improves continuity but has not
resolved source timing or established smooth display pacing.

System commit headroom during complete generating batches is only 0.3–1.7 GiB,
while physical RAM still has 6.6–8.1 GiB free. These values do not establish hard
paging: the logged high fault rate measures emulator page protection handling,
not Windows disk-backed faults. A later 320-leaf unmap takes 3.7 ms versus the
isolated 58.7 ms event, so the larger event is not a universal mapping regression.
The next diagnostic only splits Direct unmap time into host/backing, physical
owner, virtual range removal and callback, with automatic-owner scan counts;
it preserves lock and rollback order. Host/backing time still includes locks
and bookkeeping and must not be labelled pure Windows API time.

Native Reflex sleep also shares the guest Vblank thread and currently occurs
between VblankBegin and VblankEnd. In the continuous interval its mean is
3.843 ms and maximum 42.061 ms, so it can delay a 60 Hz guest tick and cause
catch-up. The source interval distribution is concentrated at 18–23 ms rather
than a clear 16.7/33.3 ms bimodal pattern; causation of perceived microstutter
remains unproven. SDK token and candidate serial are separate and correctly
matched. Host/native activity counters are not actual display spacing. A bounded
PID-filtered PresentMon display trace and separate system page-in/out counters
are prepared for the next run; neither changes the pagefile or driver settings.

### 2026-11-09 candidate: let submissions continue during host presentation

Diagnostic run `20261109-014041-9533a393` exited 0 after the player reproduced
stutters. In its 90.114-second continuously generating interval, application
and generated counts both increase by 3,932, with no on/off transition. The
source interval P99 is 49.393 ms, with 39 intervals at least 50 ms and four at
least 100 ms (maximum 158.298 ms). The bounded Reserve diagnostics directly
observe CPU submissions waiting 8–17 ms for the port configuration mutex;
driver lookup and the queue mutex are approximately zero in those examples.
The previous Flip implementation holds this port mutex across host acquisition,
Reflex sleep and presentation, blocking preparation of the following frame.
Sixteen complete active summaries cover 3,527 submissions over 80.178 seconds:
207 total Reserve calls take at least 5 ms, weighted mean port wait is 0.715 ms,
maximum port wait is 67.106 ms, and maximum queue wait is 0.018 ms. The 243 slow
detail records inside the wider active interval precede diagnostic exhaustion;
their port-wait P50/P95 are 9.474/16.452 ms. Only six of the 39 source intervals
at least 50 ms have a slow Reserve for the same request, so removing this wait
does not explain or promise to eliminate every long stall.

Flip now snapshots the admitted group's immutable layers under its port locks,
then releases those locks in reverse bus order for host Present. It keeps the
requests in Presenting state and retains m_processing, so Cancel still waits
for the actual consumer. Completion reacquires ports in bus order before the
queue mutex. If Close invalidated a generation during Present, the consumed
request is retired without changing flip status, triggering an old flip event
or discarding its image twice. Close still prevents reopen until Cancel returns
and clears the displayed layer afterwards. The not-due path retains its Ready
rollback. Guest Vblank timing, queue capacity, FIFO order and Reflex settings
are unchanged. This targets a measured serialization point; Vblank and host
presentation still share a thread, and not all source stalls are explained.

The same run's PID-filtered PresentMon trace captured one DXGI presentation
proxy in Composed: Flip mode. Within that active interval it has 7,860 rows,
of which 7,653 have display timing and 207 do not. The 7,651 fully bounded
display intervals average 11.774 ms (about 84.93 updates/second), with
P95/P99 25.019/31.313 ms, six intervals at least 50 ms and a maximum of
137.527 ms. This confirms uneven display cadence despite ongoing generation.
All rows are tagged Application, so the trace cannot distinguish generated
from real frames here; SDK generation counts remain a separate observation.
These are OS display events, not physical screen measurements. NA timings are
excluded rather than treated as zero. QPC-derived adjacent display times match
the reported MsBetweenDisplayChange column.

System page-in activity overlaps some stalls but is absent in several other
long gaps; page-out and page-write rates remain zero throughout the complete
active samples. Page-ins can read mapped assets as well as pagefile data and
the counters are system-wide. Neither these counters nor low commit headroom
establish paging as the cause of all stutters. No pagefile, driver or display
configuration was changed. The port-lock candidate builds and passes the GPU
command-lane and scheduler-timeline tests. Eight CPU lifecycle cases also pass,
using the actual extracted Reserve/Cancel/Complete/Flip/Close/SetFlipMaster
methods with a blocking host-Presenter substitute. Restoring the held port lock
fails the submit-during-Present case; removing the completion generation guard
fails the Close case. These negative controls verify that the tests detect the
targeted regressions. They do not execute real GPU preparation, the FramePool
or platform mutex implementations; those ownership paths received separate
code review.

Run `20261109-020202-56b2180d` then exited 0, but the player still reported many
stutters. In 75.037 seconds of continuous native generation, application and
generated counts both increase by 3,331, with no mode switch and one reset.
Fourteen complete Reserve summaries cover 3,111 requests over 70.120 seconds:
none take 5 ms, maximum total Reserve is 0.078 ms and maximum port wait rounds
to 0.000 ms. The targeted port blocking is removed. Source P99 falls to
35.348 ms, with ten intervals at least 50 ms and one at least 100 ms. Memory
headroom is also higher than the baseline, so the whole-game improvement cannot
be attributed to this code change alone.

Display cadence remains uneven: about 87.02 updates/second, P95 25.014 ms,
P99 31.258 ms, and two long intervals of 79.203 and 110.431 ms. In nine complete
five-second windows with every source interval below 40 ms, interval coefficient
of variation is 0.213 for the source, 0.504 for proxy Present and 0.563 for final
display; display P99 remains 31.252 ms. The precise QPC windows and matching
baseline are retained in the local `run-020202-versus-014041-display-pacing.json`.
This separates persistent pacing variation from occasional long source stalls.
It does not identify which output is generated or prove a fixed alternating
real/generated phase: lag correlations are weak, and frame-type tags do not
distinguish those outputs.

Both long display intervals overlap large system page-in rates, but commit
headroom is now 5.75–6.35 GiB and no page writes are observed in complete active
samples. Their corresponding kernel windows have no automatic-owner scans and
maximum single unmaps of 4.821 and 2.583 ms; five-second aggregates cannot assign
multiple smaller batches to the exact stalls. APR's synchronous path performs
destination invalidation, host file reads and tracked guest-memory copies under
its execution mutex. Existing logs do not measure those stages separately, so
changing read order, bypassing invalidation or blaming the pagefile is not
justified.

The shared presentation loop has another measurable boundary: a Ready request
can miss its once-per-vblank Flip poll, while Complete's condition-variable
signal cannot wake the ordinary sleep. Reflex also contributes to the same
deadline accumulator. This permits up to approximately one extra normal
60 Hz polling interval; its actual contribution requires Ready-to-admission
timing. Switching directly to Ready-driven Flip would change flip-rate and
guest-event semantics, including allowing multiple flips in one vblank, so no
such scheduler change is included. The user has deferred further playtests;
no more game windows are launched during the subsequent investigation.

The follow-up diagnostic keeps scheduling and I/O behavior unchanged. With
`KYTY_GT7_PERF_DIAGNOSTICS=1`, VideoOut emits five-second fixed-bin histograms
for Ready-to-host-Present admission, the published prepared timestamp to Ready,
host Present call duration/start spacing, VblankEnd spacing, and the existing
deadline accumulator's lateness/debt. Request and interval endpoints identify
reports crossing an active/standby boundary. The prepared timestamp follows
the existing preparation diagnostic output; it is not the start of guest GPU
rendering. Host call time is CPU time, not display time. Vblank statistics reset
when paused; Flip interval reports can still bridge a pause, so analysts must
exclude those using their recorded endpoints.

APR records queue residence/engine lookup, the initial execution-lock wait,
and setup/invalidation/host-read/guest-copy phases. It reports after the original
execution and completion routines release their locks. Slow task details are
limited to one per second and 64 over the process lifetime; five-second TLS
summaries remain available. Setup includes chunk allocation, while optional
hashing, loop overhead, explicit guest waits and completion notifications mean
phase sums do not equal the entire task. Later execution-lock reacquisitions
are not included in the initial-lock metric. Timing preserves Windows last-error
state. Disabled diagnostics add no clocks, formatting or resource traversal.
These additions are measurements for the next reproduction, not another
demonstrated stutter fix.

The diagnostic executable and matching symbols were built and installed only
in the local playtest directory. The GPU command-lane and scheduler-timeline
tests passed, as did all eight extracted VideoOut lifecycle cases; both
intentional negative controls failed as expected. Existing APR/AMM file-read,
ordering and completion-notification cases passed with diagnostics both on
and off using `kernel_file_system_tests --apr-ordering-only`. The default
filesystem suite still fails an unrelated existing path-length assertion:
the fixture expects a 1024-byte limit for the concatenated path, whereas the
unchanged resolver uses a 2048-byte buffer. The assertion and default suite
were preserved; the focused entry does not count as a full-suite pass.
The launcher's `-CheckOnly` preflight passed and selected the latest completed
`20261109-020202-56b2180d` save and caches. No new gameplay or graphics-startup
test was run while the user was away. The visible-stutter acceptance test
remains failed, and these diagnostics have not yet collected gameplay data.

### Follow-up measurement: 20261109-024227

The user returned for one further exterior-view Nurburgring reproduction with
the diagnostic candidate `a5ce6d4b`; game and bounded display capture both
exited normally. In the strict 140.294-second continuous-generation interval,
application and generated counters both increased by 6267 with no on/off
transitions and four temporal resets. Earlier startup transitions are excluded.
Reserve blocking remained absent: 6069 requests in complete reports, none at
least 5 ms, and a maximum total of 0.020 ms.

The new timing separates readiness from presentation: 27 complete windows
contain 6076 groups, with mean Ready-to-admission 8.863 ms versus mean
prepared-to-Ready 1.051 ms. Admission intervals concentrate around 16–20 ms
(63.79%) and 25–50 ms (32.69%), although source intervals mostly occupy
20–25 ms. Host Present calls average 6.043 ms; independent native timing
reports Reflex sleep averaging 5.681 ms and peaking at 35.097 ms, while the
actual Vulkan Present call averages 0.062 ms. Vblank spacing averages
16.669 ms, but 13.506% of loops finish with deadline debt and 396 of 8115
loops have End spacing at most 0.1 ms. These measurements support a
60-Hz admission sampling delay coupled to blocking Reflex work on the clock
thread; aggregates do not establish that this explains every visible stall.

PresentMon reports about 87.76 displayed updates/second, P95 25.007 ms and
P99 29.204 ms. There are nine display intervals at least 50 ms, all overlapping
slow source intervals. Eleven complete five-second windows without source
intervals above 40 ms still show source CV 0.162, proxy Present CV 0.490 and
display CV 0.553. Neither sustained generation nor average output rate proves
smooth motion. FrameType remains Application for the proxy and cannot identify
which particular display samples are generated.

APR has no read failures in the ten complete common-interval reports: 633
tasks and 1.118 GB, initial execution-lock wait at most 0.001 ms, full task at
most 27.601 ms and individual host read at most 4.834 ms. Queue residence can
reach 491.466 ms during batches; logs do not prove a renderer dependency on
those specific tasks. Five long source intervals overlap measured APR work,
which accounts for only part of those gaps. No changes to I/O ordering or
destination invalidation are justified. System commit headroom is 5.02–6.34
GiB and observed page writes are zero; system-wide page-in is not process-level
evidence of pagefile thrashing.

The next local experiment separates host presentation from the guest clock:
one serial host worker consumes Ready groups, and the 60-Hz clock publishes
only already completed flip events between Begin and End. It must retain FIFO,
per-generation/per-port admission and publication limits, flip-rate eligibility,
and Close/stop ownership. In particular, Close cannot wait for the clock to
consume completion metadata while holding the driver mutex. This experiment
is opt-in via `-AsyncNativePresentation` / `KYTY_GT7_NATIVE_ASYNC_PRESENT=1`,
requires native NVIDIA FG, and is not enabled by the launcher preset.
The experiment is implemented in local candidate `4035a1c6`. Completed requests
retain only metadata, and the host worker can pass those records while preserving
the ordering of requests still waiting to render. The guest clock checks the
owner's flip-rate eligibility again when publishing, including host calls that
span ticks. Pausing freezes guest counters; resume starts a fresh deadline.
An immutable port snapshot avoids racing Close/reopen during publication sorting.
Clock-before-host construction also permits safe unwinding if thread creation
fails. The default desktop/VR presentation path remains selected without the
explicit experiment switch.

Offline validation passed: emulator and GPU test targets built, GPU command-lane
and scheduler-timeline checks passed, all 16 extracted production-method lifecycle
cases passed, and all four targeted negative controls failed their expected
assertions. The cases cover Ready wakeups, blocked host presentation with advancing
guest ticks, admission/publication limits, master/slave rates, close/reopen,
completed metadata cancellation, pause and stop. An initial negative-control run
timed out because it continued into unrelated asynchronous cases with intentionally
broken locking; the runner now executes each mutant's intended case and still
requires its exact expected failure. The fixed implementation always runs all cases.
These CPU tests substitute presentation, timing and platform synchronization;
they do not validate real OS deadline accuracy, SDK pacing or visual quality.

Independent review found no remaining blocking issue in the tested source
(`videoOut.cpp` SHA-256 `665db5e804967d7b0d40095c00b1c3caccc1a9d2857b82ef918d878b11b61cc2`).
Only the local playtest executable, symbols and link map were installed with
matching build hashes. `-CheckOnly -AsyncNativePresentation` selected the latest
024227 save and caches and passed without creating a game session. No gameplay
or graphics-startup test has run for this scheduling candidate; its effect on
stutter remains unverified and the public release has not changed.

### Proposed 30-to-60 mode

A fixed 30-real-frame/60-presented-frame mode is not enabled in this build.
The supported Streamline cap is `ReflexOptions::frameLimitUs`; however, our
present thread also delivers guest vblank events, so sleeping there can affect
guest timing. Dropping alternate rendered frames is also invalid: the supplied
motion vectors reference the immediately preceding guest frame, and native FG
resets when the guest-frame sequence is discontinuous. A correct mode must
preserve game timing and consecutive temporal inputs. One or two queued 30 Hz
frames cover only 33–67 ms while adding input latency, and cannot hide the
observed 300–466 ms gaps.

An alternative experimental cap can reuse the existing flip interval at 60 Hz:
retain the same FIFO request until the next eligible vblank, without dropping
real frames or lowering the guest vblank clock. This avoids adding Reflex sleep
to the shared vblank/present thread. It still requires checks for catch-up
bursts, growing queued-frame latency and actual generated presentations; it is
not enabled or validated as a 30-to-60 mode yet.


## Source-frame investigation (2026-11-12)

Priority is the source-frame production path. The default-off async presentation experiment remains available, but is not combined with this diagnosis. No new gameplay result or stutter fix is claimed.

The completed run `20261109-024227-4ba86c44` has 27 consecutive MAIN preparation-start gaps >=50 ms in the strict continuous-generation window. Six gaps have exact Reserve/Prepare/Ready witnesses. The frozen a5ce6d4b executable matches the run manifest and its accompanying VideoOut snapshot reserves the CPU flip on the guest calling thread before enqueue. For these six requests Reserve-to-Prepare is only 0.015–0.046 ms, and inner Prepare-to-Ready is 0.361–0.534 ms. Thus the long gap precedes submission of that flip; earlier guest/GPU dependencies remain possible. The other 21 gaps lack the same exact witnesses.

The 117.570 ms gap ending at seq8107 shares aggregate windows with a CPU Process maximum of 86.808 ms and a tracked-memory fault maximum of 85.1 ms. These are candidates, not a proven event match. Process is CPU wall time including synchronization, and tracked faults are emulator memory-coherence handling, not Windows disk/hard-page-fault timing. APR and ordinary pipeline wait measurements do not explain this whole gap.

`-PerformanceDiagnostics -SourceDiagnostics` enables `KYTY_GT7_SOURCE_TRACE=1` only in the local playtest child. It preserves rendering/synchronization and disables the async presentation experiment by argument validation. Source trace uses a preallocated 4096-slot multiple-producer/single-consumer ring and a 120000-attempt budget. Producers do no formatting, file I/O, allocation, or waits. Full or contested slots drop records. The existing Perf reporter drains every 250 ms; output and summaries disclose loss and exhaustion. `KYTY_PERF=0` leaves source tracing disabled. Normal launch defaults are unchanged.

Record clock is steady-clock nanoseconds, compatible with existing steady timestamps. Records may be emitted out of order; sort by timestamps. The shutdown tail is incomplete: 250 ms is the normal drain interval, not a hard upper bound under OS/logging delay. Aggregate counters are concurrent snapshots, so only quiescent totals are exact.

Event schema:

- Kind 0 FlipSubmit: entry before API logging, reservation-return in work_end, enqueue-return in end; request ID joins Prepare/Ready. Success-only CPU submissions. Handle/index in arg0/arg1. Enqueue return can follow Prepare on another thread; a negative difference is valid concurrency.
- Kind 1 FlipPrepare: inner frame preparation interval after queue/port/prework, not whole GPU-lane residence. Flags source bit0, frame bit8, hold bit9, special bit10; bus/index args. Source mode replaces the old synchronous per-frame preparation text, including its periodic summary, with these records.
- Kind 2 FlipReady: callback-observed Ready instant; not exact hardware completion. Bus/index args. No retained renderer pointer.
- Kind 3 Process: completed CPU processing slices >=10 ms with stable submission ID across resumes; flags type low8, complete bit8, yielded bit9, progressed bit10; queue and first-slice args.
- Kind 4 Command: completed injected CPU commands >=10 ms; flags 1 inline/0 outer; queue/type args. Nested commands may overlap Process and must not be double-counted.
- Kind 5 Fault: successful coherence/clean-load/clean-store handling >=10 ms including statistics tail. begin/work_end/end separate actual handling from AddFault/RecordTrackedFault accounting. Flags access low8, route bits8–15, GPU lane bit16, submission type bits24–31; address range class arg0, phase low32 and queue high32 of arg1. No raw address or memory contents. Thread and active submission identify same-thread nesting; time overlap on other threads alone is not causality.

This first trace deliberately does not instrument every draw or add GPU timestamp queries. Completed short slices, unsignaled/incomplete waits, guest game logic, and the root cause inside a slow coherence handler require further evidence. The existing tracked-fault ranking lock/sorts remain unchanged to avoid altering the suspected path before measuring it.

Validation: emulator and GPU regression target build succeeded; bounded-ring concurrent payload/overflow tests, scheduler timeline and GPU command lane tests passed (3/3). Extracted actual VideoOut lifecycle tests passed (16 cases), with all four targeted negative controls failing the intended assertion. No game launched, no public release changed; performance and visual acceptance remain untested.

### Completed source trace and next comparison

Run `20261112-010759-860df983` exited normally with candidate `2bb8d595`.
In its strict 160.167-second continuous-generation interval, source preparation
has 25 gaps at least 50 ms, four at least 100 ms, and a maximum of 132.514 ms.
All 25 spend at least 99.880% of the interval before the next CPU FlipSubmit
entry. Submit-entry to inner preparation is 0.0152–0.0953 ms; preparation to
Ready is 0.3489–2.4088 ms. Native status counters increase by 7432 application
and 7432 generated frames with no on/off transition over enclosing clean
status boundaries. Sustained generation does not remove these source gaps.

This localizes the delay before that flip submission, but does not establish
which guest computation or earlier dependency caused it. The largest completed
Process is 24.997 ms and tracked fault is 17.708 ms in this interval; none of
those slow faults is on the submitter or GPU processing thread. The earlier
85-ms aggregate fault lead was not reproduced here. Do not interpret concurrent
worker intervals or APR queue residence as causal CPU cost.

The next local test explicitly disables both NVIDIA and FSR frame generation,
as requested, retaining DLSS preset M, 1920x1080 input and 2560x1440 output,
latest save, and shader caches. Use the local wrapper with
`-DLSSOnly -PerformanceDiagnostics -SourceDiagnostics -SampleSourceThreads`.
No 30-FPS cap or other graphics setting is added. No public preset changes.

`-SampleSourceThreads` adds opt-in timed stack records for the actual observed
CPU flip caller, even when unnamed, and the selected GPU/guest job threads.
It requires source diagnostics; competing aggregate sampling modes are rejected.
Capture defaults to 50 Hz, reporting every second, with 120000 attempted
captures, at most 64 live targets, 256 buffered records per target and 4096
symbol dictionary entries. Symbols and copied-stack unwinding occur after
the target resumes. Live handle identity, target generation and creation time
distinguish target lifetimes; names expose whether they were verified. Failed
captures, drops, capture/unwind costs, report intervals and the potentially
missing shutdown tail are explicit. Sampling can perturb timing.

Sample capture brackets must fit wholly inside a gap for point attribution;
boundary overlaps and failed captures remain separate. Observed stacks are
point samples, not duration coverage or proof of causality. This diagnostic
adds no rendering or synchronization fix. Build and five targeted tests passed:
source trace, source sampler disabled, unnamed producer handoff, GPU command
lane and scheduler timeline. Independent static review and parser synthetic
checks passed. Gameplay with the new sampler remains untested.

### Frame generation disabled: completed lap (2026-11-12)

Local candidate `4c8ff2d4` completed run `20261112-232745-d8e2d2ff` with exit 0.
The manifest explicitly sets NVIDIA FG and FSR FG to zero, retains DLSS SR,
and selects the previous completed save and shader caches. FSR reports disabled
at startup; no native FG activity/status is present. The user reports frequent
stutter throughout the lap despite frame generation being off.

A conservative 235.043-second continuous 3D segment contains 42 source-frame
gaps of at least 50 ms, including nine at least 100 ms and a maximum of
165.062 ms. All 150 producer points within the pre-Submit portions contain
`PthreadCondWait`, but so do almost all ordinary-frame producer points. This
identifies a waiting state, not the condition owner, notifier or root cause.
Four smaller gaps also have 19.93–35.71 ms after Submit before preparation,
with the GPU command worker executing bindings/draw/dispatch work.

The timed sampler reaches its fixed 120000-attempt cap after 307.361 seconds,
drains and stops without capture failures or drops. A later 345.074-second
trace-only 3D interval still contains 42 gaps at least 50 ms, maximum 171.650 ms.
These are different portions of the run, not a matched performance benchmark.
Individual capture brackets in the selected sampled segment are at most
0.4294 ms. Direct sampler pauses do not explain the measured long gaps;
indirect observation effects remain unmeasured.

Worker stacks identify several different paths: SPIR-V validation, GPU-memory
readback/synchronization and Windows backing-view unmap. A slow synchronous
pipeline build also shares a later aggregate reporting window with a source
gap. Some time spans overlap precisely, others only share aggregate windows;
neither establishes which dependency released the waiting producer. No broad
coherency bypass, conditional-wait change or speculative cache rewrite is
justified by these observations. The next diagnostic records producer wait
and notification timestamps directly and disables timed thread sampling.

Run evidence and reproducible joins are retained in the local analysis files
`run-232745-source-sample-audit.json`, `run-232745-observer-audit.json` and
`run-232745-cpu-stack-observations.json`. Exact race start/lap-end markers are
not logged; the selected intervals are supported by 3D scene counters.

### Exact producer condition records (local diagnostic, 2026-11-13)

Source event kinds 6 (`CondWait`) and 7 (`CondWake`) use opaque wait IDs in a
separate namespace from flip request IDs. They trace the observed OS producer's
ordinary `PthreadCondWait`; timed APIs only mark nested ambiguity. Kind 6 begins
before guest-mutex unlock, captures readiness with the existing condition mutex
held in `work_end`, and ends after guest-mutex reacquisition. `arg0` is poll
returns and `arg1` is native errno before guest error mapping. Flag low8 is API1,
bit8 is unlock-aborted (no reacquisition), and bit9 is ambiguous nested/borrowed
identity. Ready-to-end includes waiter cleanup, condition unlock and scheduling,
so it must not be described as pure guest-mutex contention.

Kind 7 timestamps immediately before `notify_one`; publication is afterward
under the same condition lock. `tid` is the notifier, `arg0` the waiter and
`arg1` its post-increment condition sequence. Flags 1/2/3 distinguish signal,
targeted signal and broadcast. Optional `submission` is the notifier's active
GPU command context, not the originating guest dependency. Notify-to-readiness
can include condition-lock reacquisition and guest signal/APC dispatch; it is
not a pure OS scheduling measure. APC-only wakeups are not condition signals.

The existing condition lock protects diagnostic payload. An atomic internal
owner tag prevents unrelated conditions from reading that payload; raw pointers
are never emitted. Nested waits permanently mark the outer occurrence uncertain
and suppress remaining notifier attribution. A detached signal helper borrowing
the guest identity marks the active occurrence uncertain and excludes future
attribution for that object, conservatively including pool reuse. This reduces
coverage rather than fabricating a dependency. Synchronization, polling, return
values and recursive mutex behavior are unchanged.

The next local entry retains DLSS SR with both FG providers off and explicitly
sets `KYTY_SAMPLE=0` and `KYTY_SAMPLE_SOURCE=0`. Condition events share the bounded
source ring and its 120000-attempt budget; dropped/missing pairs and process tail
remain incomplete evidence. Focused helper tests cover notifier identity,
reacquisition boundaries, nested/borrowed suppression and event lifetime; they
do not claim full native-pthread integration or a gameplay stutter fix.
