[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$GamePath,
    [Parameter(Mandatory=$true)][string]$EmulatorPath,
    [Parameter(Mandatory=$true)][string]$BackendPath,
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [Parameter(Mandatory=$true)][string]$SourceUserDataRoot,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [string[]]$ProgressSearchRoots,
    [switch]$Baseline,
    [switch]$NativePssr,
    [switch]$NativePssr1080To4K,
    [switch]$NativePssrDiagnostics,
    [switch]$FrameGeneration,
    [switch]$NativeFrameGeneration,
    [switch]$AsyncNativePresentation,
    [string]$StreamlineRuntimeDirectory,
    [ValidateSet('K','L','M')][string]$DlssPreset = 'K',
    [switch]$OcclusionGuard,
    [switch]$VulkanValidation,
    [switch]$CaptureTemporalInputs,
    [switch]$CaptureGeneratedFrames,
    [switch]$DiagnosticLogging,
    [switch]$PerformanceDiagnostics,
    [switch]$SourceDiagnostics,
    [switch]$SourceWaitChain,
    [switch]$SourceEventBridge,
    [switch]$SampleSourceThreads,
    [switch]$SampleSourceNotifier,
    [switch]$SampleGpuThread,
    [switch]$SampleProducerThreads,
    [string]$DeviceCreationReport,
    [switch]$CheckOnly,
    [switch]$GraphicsCheckOnly
)
# Local developer playtest. Input capture uses F9; separate FG output capture uses F10.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'GT7-LaunchSupport.ps1')
. (Join-Path $PSScriptRoot 'GT7-PlaytestState.ps1')
try {
    if ($CheckOnly -and $GraphicsCheckOnly) {
        throw '-CheckOnly and -GraphicsCheckOnly are mutually exclusive.'
    }
    if ($NativePssrDiagnostics -and (-not $NativePssr -or $GraphicsCheckOnly)) {
        throw '-NativePssrDiagnostics requires -NativePssr and a game session.'
    }
    if ($NativePssr1080To4K -and -not $NativePssr) {
        throw '-NativePssr1080To4K requires -NativePssr.'
    }
    if ($NativePssr -and ($Baseline -or $FrameGeneration -or $CaptureTemporalInputs -or $CaptureGeneratedFrames)) {
        throw '-NativePssr cannot be combined with Baseline, FSR frame generation, or legacy input/output capture. Use -NativeFrameGeneration explicitly for fixed 2X NVIDIA FG.'
    }
    if ($FrameGeneration -and $NativeFrameGeneration) {
        throw '-FrameGeneration and -NativeFrameGeneration are mutually exclusive.'
    }
    if ($AsyncNativePresentation -and -not $NativeFrameGeneration) {
        throw '-AsyncNativePresentation requires -NativeFrameGeneration.'
    }
    if ($Baseline -and ($FrameGeneration -or $NativeFrameGeneration)) {
        throw '-Baseline and frame generation are mutually exclusive.'
    }
    if ($OcclusionGuard -and -not $FrameGeneration) {
        throw '-OcclusionGuard requires -FrameGeneration.'
    }
    if ($SampleGpuThread -and (-not $PerformanceDiagnostics -or $GraphicsCheckOnly)) {
        throw '-SampleGpuThread requires -PerformanceDiagnostics and a game session.'
    }
    if ($SourceDiagnostics -and (-not $PerformanceDiagnostics -or $GraphicsCheckOnly -or $AsyncNativePresentation)) {
        throw '-SourceDiagnostics requires -PerformanceDiagnostics and a game session, without -AsyncNativePresentation.'
    }
    if ($SourceWaitChain -and -not $SourceDiagnostics) {
        throw '-SourceWaitChain requires -SourceDiagnostics.'
    }
    if ($SourceEventBridge -and (-not $SourceWaitChain -or $SampleSourceThreads -or $SampleSourceNotifier -or $SampleGpuThread -or $SampleProducerThreads)) {
        throw '-SourceEventBridge requires -SourceWaitChain and cannot be combined with timed thread sampling.'
    }
    if ($SampleSourceThreads -and (-not $SourceDiagnostics -or $SampleGpuThread -or $SampleProducerThreads -or $SampleSourceNotifier)) {
        throw '-SampleSourceThreads requires -SourceDiagnostics and cannot be combined with the other sampling switches.'
    }
    if ($SampleSourceNotifier -and (-not $SourceDiagnostics -or $SampleGpuThread -or $SampleProducerThreads -or $SampleSourceThreads)) {
        throw '-SampleSourceNotifier requires -SourceDiagnostics and cannot be combined with the other sampling switches.'
    }
    if ($SampleProducerThreads -and (-not $PerformanceDiagnostics -or $GraphicsCheckOnly -or $SampleGpuThread)) {
        throw '-SampleProducerThreads requires -PerformanceDiagnostics and a game session, without -SampleGpuThread.'
    }
    if ($CaptureTemporalInputs -and $GraphicsCheckOnly) {
        throw '-CaptureTemporalInputs requires a game session and cannot be combined with -GraphicsCheckOnly.'
    }
    if ($CaptureGeneratedFrames -and (-not $FrameGeneration -or $GraphicsCheckOnly -or $CaptureTemporalInputs)) {
        throw '-CaptureGeneratedFrames requires -FrameGeneration and cannot be combined with -GraphicsCheckOnly or -CaptureTemporalInputs.'
    }
    $exe = (Resolve-Path -LiteralPath $EmulatorPath).ProviderPath
    $backend = (Resolve-Path -LiteralPath $BackendPath).ProviderPath
    $runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).ProviderPath
    $source = (Resolve-Path -LiteralPath $SourceUserDataRoot).ProviderPath
    $game = (Resolve-Path -LiteralPath $GamePath).ProviderPath
    if (Test-Path -LiteralPath $game -PathType Container) { $game = Join-Path $game 'eboot.bin' }
    foreach ($file in @($exe, $backend, $game, (Join-Path $runtime 'nvngx_dlss.dll'))) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing required file: $file" }
    }
    $frameGenerationRuntime = Join-Path (Split-Path -Parent $exe) 'amd_fidelityfx_vk.dll'
    if ($FrameGeneration -and -not (Test-Path -LiteralPath $frameGenerationRuntime -PathType Leaf)) {
        throw "Missing frame generation runtime: $frameGenerationRuntime"
    }
    $streamlineRuntime = $null
    $streamlineRuntimeFiles = @('sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll','nvngx_dlssg.dll','NvLowLatencyVk.dll')
    if ($NativeFrameGeneration) {
        if ([string]::IsNullOrWhiteSpace($StreamlineRuntimeDirectory)) {
            throw '-NativeFrameGeneration requires -StreamlineRuntimeDirectory.'
        }
        $streamlineRuntime = (Resolve-Path -LiteralPath $StreamlineRuntimeDirectory).ProviderPath
        # Streamline initializes NGX before the native PSSR SR session. Both must
        # share the same runtime identity, including nvngx_dlss.dll, in that process.
        if ($NativePssr -and -not [StringComparer]::OrdinalIgnoreCase.Equals(
                $runtime.TrimEnd([char[]]'\/'), $streamlineRuntime.TrimEnd([char[]]'\/'))) {
            throw '-NativePssr with -NativeFrameGeneration requires RuntimeDirectory and StreamlineRuntimeDirectory to resolve to the same directory containing both DLSS SR and NVIDIA FG runtimes.'
        }
        foreach ($name in $streamlineRuntimeFiles) {
            if (-not (Test-Path -LiteralPath (Join-Path $streamlineRuntime $name) -PathType Leaf)) {
                throw "Missing native frame generation runtime: $name"
            }
        }
    }
    $progress = $null
    if (-not $GraphicsCheckOnly) {
        $progress = Select-GT7PlaytestState -SourceUserDataRoot $source -OutputRoot $OutputRoot -ProgressSearchRoots $ProgressSearchRoots
        Write-Host "Progress source ($($progress.selection)): $($progress.selected_root)"
        Write-Host "GT7 save: $($progress.save_path); SHA-256: $($progress.save_sha256)"
        Write-Host "Shader cache source ($($progress.shader_cache_selection)): $($progress.shader_cache_source)"
        Write-Host "Driver pipeline cache source ($($progress.driver_cache_selection)): $($progress.driver_cache_source)"
    }
    # Full per-API traces can grow by tens of GiB per hour even in menus.
    # Ordinary play keeps fatal messages in stdout without the detailed trace.
    $traceEnabled = [bool]($DiagnosticLogging -or $CaptureTemporalInputs)
    $logDirection = $(if ($traceEnabled) { 'File' } else { 'Silent' })
    Write-Host "Logging mode: $logDirection (detailed trace opt-in; fatal output is still retained)."
    $nativeFgNote = $(if ($NativeFrameGeneration) {
        'Fixed 2X NVIDIA frame generation is requested (one generated frame per real frame). Runtime support and valid same-frame inputs gate generation; requesting it does not prove generated or displayed frames. Inspect native FG status/activity counters separately from game FPS.'
    } else { 'Frame generation remains disabled.' })
    if ($NativePssr1080To4K) {
        Write-Host "Requested graphics: GT7 Pro 1920x1080 scene -> native PSSR -> DLSS 3840x2160, preset=$DlssPreset; window=3840x2160. $nativeFgNote"
        Write-Host 'The scene-resolution patch is requested; confirm actual input/depth/motion and output dimensions in the native PSSR diagnostics.'
    } elseif ($NativePssr) {
        Write-Host "Requested graphics: native PSSR -> DLSS, preset=$DlssPreset; render/output sizes come from the game, window=2560x1440. $nativeFgNote"
    } else {
        Write-Host "Requested graphics: DLSS SR=$(-not [bool]$Baseline), preset=$DlssPreset, 1920x1080 -> 2560x1440; NVIDIA FG=$([bool]$NativeFrameGeneration), FSR FG=$([bool]$FrameGeneration)."
    }
    if ($NativePssrDiagnostics) { Write-Host 'Native PSSR automatic input/output diagnostics enabled; no F9/F10 capture is required. This session is for image diagnosis, not performance measurement.' }
    Write-Host "Source diagnostics: timing=$([bool]$SourceDiagnostics), condition chain=$([bool]$SourceWaitChain), timed thread sampling=$([bool]$SampleSourceThreads), notifier-only sampling=$([bool]$SampleSourceNotifier)."
    if ($SourceEventBridge) { Write-Host 'Event bridge diagnostics: manually create the run _source-trace.arm marker after race entry; trace budgets remain idle until armed.' }
    $windowWidth = $(if ($NativePssr1080To4K) { '3840' } else { '2560' })
    $windowHeight = $(if ($NativePssr1080To4K) { '2160' } else { '1440' })
    $arguments = @('--game', $game, '--screen-width', $windowWidth, '--screen-height', $windowHeight,
        '--present-mode', 'Mailbox', '--tessellation', '--bindless', '--redzone',
        '--shader-optimization-type', 'Performance', '--console-language', '11',
        '--readback-linear-images', 'false', '--printf-direction', $logDirection,
        '--printf-output-file', '_kyty.txt', '--gt7-desktop-height', $(if ($NativePssr) { 'native' } else { '1080' }), '--user-id', '1000')
    if ($NativePssr) { $arguments += @('--ps5-pro', '--pssr-dlss') }
    if ($NativePssr1080To4K) { $arguments += '--gt7-pssr-1080-4k' }
    $check = Invoke-GT7CompatibilityCheck -Executable $exe -ArgumentList $arguments `
        -WorkingDirectory (Split-Path -Parent $exe)
    if ($check.ExitCode -ne 0) { throw "GT7 preflight failed: $($check.StandardOutput) $($check.StandardError)" }
    if ($CheckOnly) {
        Write-Output 'Playtest preflight passed. No game process or playtest directory created.'
        exit 0
    }
    # Keep the compatibility preflight free of the graphics flag: --gt7-check-only
    # returns before device initialization, so combining both would hide the check.
    if ($GraphicsCheckOnly) { $arguments += '--graphics-check-only' }
    if ($VulkanValidation) { $arguments += @('--vulkan-validation', 'true') }
    $run = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
    [void][IO.Directory]::CreateDirectory($run)
    if (-not $GraphicsCheckOnly) { Copy-GT7PlaytestState -Selection $progress -DestinationRoot $run }
    $settings = [ordered]@{
        KYTY_GT7_DLSS = $(if ($Baseline -or $NativePssr) { '0' } else { '1' })
        KYTY_PSSR_DIAGNOSTICS = $(if ($NativePssrDiagnostics) { '1' } else { '0' })
        KYTY_GT7_DLSS_BACKEND = $backend
        KYTY_GT7_DLSS_RUNTIME = $runtime
        KYTY_GT7_DLSS_PRESET = $DlssPreset.ToUpperInvariant()
        KYTY_GT7_DLSS_FG = $(if ($NativeFrameGeneration) { '1' } else { '0' })
        KYTY_GT7_NATIVE_ASYNC_PRESENT = $(if ($AsyncNativePresentation) { '1' } else { '0' })
        KYTY_GT7_SOURCE_TRACE = $(if ($SourceDiagnostics) { '1' } else { '0' })
        KYTY_GT7_SOURCE_CHAIN = $(if ($SourceWaitChain) { '1' } else { '0' })
        KYTY_GT7_EVENT_TRACE = $(if ($SourceEventBridge) { '1' } else { '0' })
        KYTY_GT7_SOURCE_ARM_FILE = $(if ($SourceEventBridge) { Join-Path $run '_source-trace.arm' } else { '' })
        KYTY_SAMPLE_SOURCE = $(if ($SampleSourceThreads -or $SampleSourceNotifier) { '1' } else { '0' })
        KYTY_SAMPLE_SOURCE_NOTIFIER = $(if ($SampleSourceNotifier) { '1' } else { '0' })
        KYTY_SAMPLE = '0'
        KYTY_GT7_FSR_FG = $(if ($FrameGeneration) { '1' } else { '0' })
        KYTY_GT7_FSR_FG_OCCLUSION_GUARD = $(if ($OcclusionGuard) { '1' } else { '0' })
    }
    if ($NativeFrameGeneration) {
        $settings['KYTY_GT7_DLSS_FG_RUNTIME'] = $streamlineRuntime
        # RTSS's implicit Vulkan layer crashes in device creation with the
        # Streamline proxy on the development driver. This is the layer's
        # documented per-process opt-out; it does not change global RTSS settings.
        $settings['DISABLE_RTSS_LAYER'] = '1'
    }
    if ($DiagnosticLogging) { $settings['KYTY_GT7_DLSS_NGX_LOG'] = '1' }
    if ($PerformanceDiagnostics) {
        $settings['KYTY_GT7_PERF_DIAGNOSTICS'] = '1'
        $settings['KYTY_PERF'] = '5'
    }
    if ($SampleGpuThread) {
        # Short diagnostic only: samples the command-producing GPU thread, not
        # all guest job threads. Sampling may perturb timings; keep it opt-in.
        $settings['KYTY_SAMPLE'] = 'Thread_Gpu'
        $settings['KYTY_SAMPLE_HZ'] = '100'
        $settings['KYTY_SAMPLE_REPORT'] = '1'
    }
    if ($SampleProducerThreads) {
        # Short CPU-side stall investigation. Lower sampling frequency limits
        # suspension overhead across guest jobs; results can perturb timings.
        $settings['KYTY_SAMPLE'] = 'Thread_Gpu,Job#*,Thread_DrawPrep*,GpuRecord,GpuPriority'
        $settings['KYTY_SAMPLE_HZ'] = '50'
        $settings['KYTY_SAMPLE_REPORT'] = '1'
    }
    if ($SampleSourceThreads) {
        # Exact capture times connect stacks to source gaps. The sampler also
        # follows the observed CPU flip caller, even if it has no registered name.
        $settings['KYTY_SAMPLE'] = 'Thread_Gpu,Job#*'
        $settings['KYTY_SAMPLE_HZ'] = '50'
        $settings['KYTY_SAMPLE_REPORT'] = '1'
    }
    if ($SampleSourceNotifier) {
        # Follow only the thread observed notifying the current CPU flip caller.
        # No named-worker fallback before discovery; capture can still perturb timing.
        $settings['KYTY_SAMPLE'] = '1'
        $settings['KYTY_SAMPLE_HZ'] = '50'
        $settings['KYTY_SAMPLE_REPORT'] = '1'
    }
    if ($DeviceCreationReport) {
        $settings['KYTY_DEVICE_CREATE_REPORT'] = [IO.Path]::GetFullPath($DeviceCreationReport)
    }
    $captureSettings = $null
    if ($CaptureTemporalInputs) {
        # Match the verified TAA in Gt7Dlss::BeginTaa and the final tone-map pass in
        # FrameGenerationTonemapMatches. Keep the 1080p depth/MV/color at mip 0,
        # row_step=1; larger history/tone-map inputs can use a mip or sampled rows.
        $captureSettings = [ordered]@{
            KYTY_FG_MANUAL_ONLY = '1'
            KYTY_FG_FRAMES = '4'
            KYTY_FG_VERIFY = '0'
            KYTY_FG_SAMPLES = '0'
            KYTY_DUMP_IMAGES = '1920x1080,3840x2160'
            KYTY_DUMP_IMAGES_SHADERS = 'c3113b114fb02e5d,edb9cdad0985bf44'
            KYTY_DUMP_IMAGES_MAX = '24'
            KYTY_DUMP_IMAGES_SIZE = '1920'
            KYTY_DUMP_IMAGES_MB = '64'
            KYTY_DUMP_IMAGES_RAW = '1'
            KYTY_DUMP_IMAGES_RAW_MB = '512'
        }
        foreach ($entry in $captureSettings.GetEnumerator()) { $settings[$entry.Key] = $entry.Value }
    }
    if ($CaptureGeneratedFrames) {
        # Bounded output pairs have their own metadata; no full API trace needed.
        $settings['KYTY_GT7_FSR_FG_CAPTURE_OUTPUTS'] = '1'
        $settings['KYTY_FG_MANUAL_ONLY'] = '1'
    }
    $manifest = [ordered]@{
        schema = 1; kind = 'gt7_dlss_experimental_playtest'; status = 'prepared'; exit_code = $null
        mode = $(if ($GraphicsCheckOnly) { 'graphics_check_only' } else { 'playtest' })
        graphics_check_only = [bool]$GraphicsCheckOnly
        diagnostic_logging_requested = [bool]$DiagnosticLogging
        native_pssr_diagnostics_requested = [bool]$NativePssrDiagnostics
        performance_diagnostics_requested = [bool]$PerformanceDiagnostics
        source_diagnostics_requested = [bool]$SourceDiagnostics
        source_condition_chain_requested = [bool]$SourceWaitChain
        source_event_bridge_requested = [bool]$SourceEventBridge
        source_trace_arm_file = $(if ($SourceEventBridge) { $settings['KYTY_GT7_SOURCE_ARM_FILE'] } else { $null })
        source_thread_sampling_requested = [bool]$SampleSourceThreads
        source_notifier_sampling_requested = [bool]$SampleSourceNotifier
        async_native_presentation_requested = [bool]$AsyncNativePresentation
        gpu_thread_sampling_requested = [bool]$SampleGpuThread
        producer_thread_sampling_requested = [bool]$SampleProducerThreads
        detailed_trace_enabled = $traceEnabled
        log_direction = $logDirection
        trace_note = 'Ordinary play uses Silent: detailed API traces are disabled; fatal messages and native NVIDIA FG periodic status/presentation counters remain in stdout.log. DiagnosticLogging or CaptureTemporalInputs enables the full trace, including detailed SR/FSR FG counters, for a short diagnostic session.'
        temporal_input_capture_requested = [bool]$CaptureTemporalInputs
        generated_frame_capture_requested = [bool]$CaptureGeneratedFrames
        generated_frame_capture = $(if ($CaptureGeneratedFrames) {
            [ordered]@{
                trigger = 'F10'; output_directory = 'fg-output-captures'
                requested_pairs_per_batch = 4; maximum_batches_per_process = 4
                matching_inputs = @('HUDless display color PNG', 'private depth R32_FLOAT raw', 'private backward pixel motion RG32_FLOAT raw')
                maximum_readback_bytes_per_batch = 276480000
                phase = 'After successful SDK generation dispatch, before presentation. Images do not prove display.'
                completion_verified = $false
                note = 'Use frame IDs, guest-frame IDs and reset metadata to establish valid comparisons. Do not press F9 in this mode. A batch may briefly pause after collecting its outputs for readback and encoding.'
            }
        } else { $null })
        temporal_input_capture = $(if ($CaptureTemporalInputs) {
            [ordered]@{
                trigger = 'F9'; requested_environment = $captureSettings
                trigger_note = 'Manual-only FrameGraph capture is requested: automatic diagnostics and scheduled captures are ignored, while the four-frame sequence started by F9 continues normally.'
                image_capture_phase = 'Before the bound shader operation. A write binding is not an after-write result.'
                raw_note = 'The verified single-layer 1920x1080 inputs are requested at full resolution without row sampling. Larger inputs may use a smaller mip or sampled rows. Inspect each raw JSON layout and the log to confirm completeness; image and 512 MiB process-wide raw limits can stop dumping.'
                completion_verified = $false
            }
        } else { $null })
        frame_generation_requested = [bool]($FrameGeneration -or $NativeFrameGeneration)
        frame_generation_provider = $(if ($NativeFrameGeneration) { 'nvidia-streamline-vulkan' } elseif ($FrameGeneration) { 'amd-fidelityfx-vulkan' } else { 'none' })
        dlss_preset_requested = $DlssPreset
        pssr_dlss_requested = [bool]$NativePssr
        native_pssr_1080_to_4k_requested = [bool]$NativePssr1080To4K
        native_pssr_target = $(if ($NativePssr1080To4K) {
            [ordered]@{input_width=1920; input_height=1080; output_width=3840; output_height=2160; verified=$false}
        } else { $null })
        window_requested = [ordered]@{width=[int]$windowWidth; height=[int]$windowHeight}
        super_resolution_input_source = $(if ($NativePssr) { 'native-psml-pssr' } else { 'gt7-taa' })
        streamline_runtime_directory = $streamlineRuntime
        occlusion_guard_requested = [bool]$OcclusionGuard
        occlusion_guard_verified = $false
        occlusion_guard_note = 'Experimental local fallback to current real pixels at large current/previous motion discontinuities. No fixed wheel rectangle. Requested state does not prove GPU execution, visual improvement or performance; inspect guard counters and capture metadata. Local temporal seams remain possible.'
        vulkan_validation_requested = [bool]$VulkanValidation
        vulkan_validation_active = $null # Confirm the enabled layer in the runtime log.
        frame_generation_requested_multiplier = $(if ($FrameGeneration -or $NativeFrameGeneration) { 2 } else { 1 })
        frame_generation_requested_generated_frames_per_real = $(if ($FrameGeneration -or $NativeFrameGeneration) { 1 } else { 0 })
        frame_generation_verified = $false # Requesting FG does not prove interpolation ran.
        frame_generation_verification_note = 'The requested multiplier is fixed at 2X when enabled. Configuration and SDK presented counts alone do not establish generated-frame delivery or monitor FPS; inspect actual FG status, generated-frame activity and presentation counters.'
        frame_generation_runtime_sha256 = $(if ($FrameGeneration) { (Get-FileHash -LiteralPath $frameGenerationRuntime -Algorithm SHA256).Hash.ToLowerInvariant() } elseif ($NativeFrameGeneration) { (Get-FileHash -LiteralPath (Join-Path $streamlineRuntime 'nvngx_dlssg.dll') -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null })
        streamline_runtime_files = $(if ($NativeFrameGeneration) {
            @($streamlineRuntimeFiles | ForEach-Object {
                [ordered]@{ name=$_; sha256=(Get-FileHash -LiteralPath (Join-Path $streamlineRuntime $_) -Algorithm SHA256).Hash.ToLowerInvariant() }
            })
        } else { @() })
        started_utc = [DateTime]::UtcNow.ToString('o'); baseline = [bool]$Baseline
        emulator = $exe; backend = $backend; runtime_directory = $runtime
        emulator_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
        backend_sha256 = (Get-FileHash -LiteralPath $backend -Algorithm SHA256).Hash.ToLowerInvariant()
        runtime_sha256 = (Get-FileHash -LiteralPath (Join-Path $runtime 'nvngx_dlss.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
        source_user_data = $source; working_directory = $run; arguments = $arguments; environment = $settings
        progress_state = $progress
        note = $(if ($GraphicsCheckOnly) {
            'Initializes the real emulator window and Vulkan device, then exits before guest ELF loading or execution. No game state copied. This does not validate DLSS evaluation or rendering.'
        } elseif ($NativePssr1080To4K) {
            "Experimental GT7 Pro scene-resolution preset requests 1920x1080 rendering and 3840x2160 native PSSR/DLSS reconstruction. The game supplies the actual color/depth/motion rectangles, jitter and motion scales; the renderer does not relabel or crop higher-resolution input to claim 1080p. Window is 3840x2160. Verify actual native request dimensions and DLSS/fallback counters; the requested preset alone does not establish in-game correctness or performance. $nativeFgNote"
        } elseif ($NativePssr) {
            "Experimental native Psml/PSSR input to the existing DLSS backend. Pro must be advertised by the title and native PSSR dispatch must occur. Render/output sizes follow the game; only the window is fixed at 2560x1440. Native mode does not use the old 1080p scene patch or TAA hook. $nativeFgNote Inspect PSSR request, DLSS evaluation and spatial fallback counters; enabling the switch is not evidence of DLSS execution."
        } else {
            "Conditional fixed 1920x1080 to 2560x1440 desktop SR with the Quality mode; explicit preset $DlssPreset is written to all six ratio-specific model hint slots for this custom 75% input ratio. Guest tone-map/UI surface remains 3840x2160, window is 2560x1440. Inspect SR evaluation/composition and the selected FG provider's activity counters separately. Requested options and SDK presented counts are not measured monitor FPS. Exposure/temporal quality and performance require in-game verification."
        })
    }
    $manifestPath = Join-Path $run 'playtest-manifest.json'
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    $process = New-Object System.Diagnostics.Process
    $stdout = $null; $stderr = $null
    try {
        $process.StartInfo = New-GT7ProcessStartInfo -Executable $exe -ArgumentList $arguments -WorkingDirectory $run -CaptureOutput
        foreach ($key in @($process.StartInfo.EnvironmentVariables.Keys)) {
            if ($key -match '^(KYTY_|GT7_)') { $process.StartInfo.EnvironmentVariables.Remove($key) }
        }
        foreach ($entry in $settings.GetEnumerator()) { $process.StartInfo.EnvironmentVariables[$entry.Key] = $entry.Value }
        $stdout = [IO.FileStream]::new((Join-Path $run 'stdout.log'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read, 1, [IO.FileOptions]::Asynchronous)
        $stderr = [IO.FileStream]::new((Join-Path $run 'stderr.log'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read, 1, [IO.FileOptions]::Asynchronous)
        Write-Host "Playtest folder: $run"
        if ($traceEnabled) { Write-Host 'Full diagnostic trace enabled: keep this session short and close normally after the required evidence is collected.' }
        if ($GraphicsCheckOnly) {
            Write-Host 'Checking real window and Vulkan device initialization. The emulator will exit before loading or running the game.'
        } else {
            if ($CaptureGeneratedFrames) {
                Write-Host 'Enter the affected external wheel view with frame generation enabled, then press F10 once while ghosting is visible.'
                Write-Host 'F10 requests four real/generated pairs plus matching HUDless color, depth and motion under fg-output-captures. A short pause after capture is expected. Do not press F9 in this mode.'
            } elseif ($CaptureTemporalInputs) {
                Write-Host 'Enter the same race and external camera view, wait for shader compilation to settle, then press F9 once while the wheel issue is visible.'
                Write-Host 'F9 captures 4 frames of temporal inputs, including full-resolution 1080p depth/motion/color. Capture can briefly pause rendering; drive a few seconds, then close normally.'
            } else {
                Write-Host 'Enter the same race/cockpit view and drive normally. No F9 capture is needed. Close normally when finished.'
            }
            Write-Host 'This is a local experimental build. Unsupported pass combinations retain the original image.'
            if ($FrameGeneration) {
                Write-Host 'Desktop 2x Vulkan FSR frame generation requested alongside DLSS SR. DiagnosticLogging is required to record FG verification counters; unsupported frames use normal presentation.'
            } elseif ($NativeFrameGeneration) {
                Write-Host 'Native Vulkan NVIDIA 2x frame generation requested. Verify SDK presented-frame activity separately from requested configuration and game FPS.'
            }
        }
        if (-not $process.Start()) { throw 'Could not start the emulator.' }
        $manifest['status'] = 'running'; $manifest['pid'] = $process.Id
        $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
        $stdoutCopy = $process.StandardOutput.BaseStream.CopyToAsync($stdout)
        $stderrCopy = $process.StandardError.BaseStream.CopyToAsync($stderr)
        $process.WaitForExit()
        $manifest['exit_code'] = $process.ExitCode
        [void]$stdoutCopy.GetAwaiter().GetResult(); [void]$stderrCopy.GetAwaiter().GetResult()
        $manifest['status'] = 'finished'
    } catch {
        $manifest['status'] = 'error'; $manifest['error'] = $_.Exception.Message
        throw
    } finally {
        if ($null -ne $stdout) { $stdout.Dispose() }
        if ($null -ne $stderr) { $stderr.Dispose() }
        $process.Dispose()
        $manifest['finished_utc'] = [DateTime]::UtcNow.ToString('o')
        $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    }
    Write-Host "Playtest finished: $run"
    exit $manifest['exit_code']
} catch {
    Write-Error $_
    exit 1
}
