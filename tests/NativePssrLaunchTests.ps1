[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$PowerShellExecutable = (Get-Process -Id $PID).Path
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scripts = Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts'
. (Join-Path $scripts 'GT7-LaunchSupport.ps1')
# Keep fixture-run save paths below the legacy PS5.1/.NET Framework path limit.
$root = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('npssr-' + [Guid]::NewGuid().ToString('N').Substring(0,12))
[void][IO.Directory]::CreateDirectory($root)
$result = [ordered]@{ passed=$false; game_started=$false; real_eboot_validated=$false;
    powershell=$PowerShellExecutable; scope='Offline arguments/dependencies and fixture-only manifest/environment; no emulator or graphics runtime loaded'; checks=@() }
try {
    # A .NET Framework console stub works under both PS5.1 and PS7 child hosts.
    # It cannot load a game or graphics runtime. Explicit stub-run cases exercise
    # the manifest/environment path; every other invocation requires check-only.
    $probeSource = @'
using System;
using System.IO;
using System.Text;
public class NativePssrArgumentProbe {
    public static int Main(string[] args) {
        bool check = Array.IndexOf(args, "--gt7-check-only") >= 0;
        if (!check && Environment.GetEnvironmentVariable("NATIVE_PSSR_TEST_ALLOW_STUB_RUN") != "1") return 97;
        using (var writer = new StreamWriter(Environment.GetEnvironmentVariable("NATIVE_PSSR_TEST_ARGUMENT_LOG"), true)) {
            writer.WriteLine("CALL");
            foreach (string arg in args) writer.WriteLine(Convert.ToBase64String(Encoding.UTF8.GetBytes(arg)));
        }
        if (!check) {
            using (var writer = new StreamWriter(Environment.GetEnvironmentVariable("NATIVE_PSSR_TEST_ENVIRONMENT_LOG"))) {
                foreach (string key in new [] {"KYTY_GT7_DLSS", "KYTY_GT7_DLSS_FG", "KYTY_GT7_FSR_FG",
                    "KYTY_GT7_DLSS_FG_RUNTIME", "KYTY_GT7_DLSS_PRESET", "KYTY_PSSR_DIAGNOSTICS",
                    "KYTY_GT7_NATIVE_ASYNC_PRESENT", "DISABLE_RTSS_LAYER"}) {
                    writer.WriteLine(key + "=" + Convert.ToBase64String(Encoding.UTF8.GetBytes(Environment.GetEnvironmentVariable(key) ?? "")));
                }
            }
        }
        return 0;
    }
}
'@
    $source = Join-Path $root 'probe.cs'
    $probe = Join-Path $root 'argument probe.exe'
    [IO.File]::WriteAllText($source, $probeSource)
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $compiler /nologo /target:exe (('/out:' + $probe)) $source
    if ($LASTEXITCODE -ne 0) { throw 'Argument stub compilation failed.' }
    $fixture = Join-Path $root 'fake game and runtime'
    $seed = Join-Path $root 'unchanged seed'
    [void][IO.Directory]::CreateDirectory($fixture)
    [void][IO.Directory]::CreateDirectory((Join-Path $seed '_DownloadData\PPSA01317'))
    foreach ($name in @('eboot.bin','backend.dll','nvngx_dlss.dll')) {
        [IO.File]::WriteAllText((Join-Path $fixture $name), 'Offline fixture only; never execute or load.')
    }
    $streamline = $fixture
    $streamlineFiles = @('sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll','nvngx_dlssg.dll','NvLowLatencyVk.dll')
    [void][IO.Directory]::CreateDirectory($streamline)
    foreach ($name in $streamlineFiles) {
        [IO.File]::WriteAllText((Join-Path $streamline $name), 'Offline fixture only; never execute or load.')
    }
    # Identical bytes in a separate directory still have a different NGX runtime identity.
    $separateStreamline = Join-Path $root 'separate streamline runtime'
    [void][IO.Directory]::CreateDirectory($separateStreamline)
    foreach ($name in @('nvngx_dlss.dll') + $streamlineFiles) {
        Copy-Item -LiteralPath (Join-Path $streamline $name) -Destination (Join-Path $separateStreamline $name)
    }
    [IO.File]::WriteAllText((Join-Path $seed '_DownloadData\PPSA01317\gt7save.bin'), 'Do not change this fake progress.')
    function SeedSnapshot {
        @((Get-ChildItem -LiteralPath $seed -Force -Recurse | Sort-Object FullName | ForEach-Object {
            $content = if ($_.PSIsContainer) { 'directory' } else { (Get-FileHash -LiteralPath $_.FullName).Hash }
            # NTFS can publish a new directory's timestamp after its first enumeration.
            $written = if ($_.PSIsContainer) { 0 } else { $_.LastWriteTimeUtc.Ticks }
            $_.FullName + '|' + $written + '|' + $content
        })) -join "`n"
    }
    $seedBefore = SeedSnapshot
    [IO.File]::WriteAllText((Join-Path $root 'seed-before.txt'), $seedBefore)
    $cases = @(
        @{Name='native-original'; Args=@('-NativePssr'); Width='2560'; Height='1440'; Preset=$false; Valid=$true},
        @{Name='native-1080-to-4k'; Args=@('-NativePssr','-NativePssr1080To4K'); Width='3840'; Height='2160'; Preset=$true; Valid=$true},
        @{Name='missing-native'; Args=@('-NativePssr1080To4K'); Valid=$false},
        @{Name='baseline-conflict'; Args=@('-NativePssr','-NativePssr1080To4K','-Baseline'); Valid=$false},
        @{Name='fsr-fg-conflict'; Args=@('-NativePssr','-NativePssr1080To4K','-FrameGeneration'); Valid=$false},
        @{Name='native-nvidia-fg-2x'; Args=@('-NativePssr','-NativeFrameGeneration','-StreamlineRuntimeDirectory',$streamline); Width='2560'; Height='1440'; Preset=$false; NativeFG=$true; Valid=$true},
        @{Name='native-1080-4k-nvidia-fg-2x'; Args=@('-NativePssr','-NativePssr1080To4K','-NativeFrameGeneration','-StreamlineRuntimeDirectory',$streamline); Width='3840'; Height='2160'; Preset=$true; NativeFG=$true; Valid=$true},
        @{Name='native-fg-resolved-same-runtime'; Args=@('-NativePssr','-NativeFrameGeneration','-StreamlineRuntimeDirectory',(Join-Path $streamline '.')); Width='2560'; Height='1440'; Preset=$false; NativeFG=$true; Valid=$true},
        @{Name='native-fg-runtime-path-case'; Args=@('-NativePssr','-NativeFrameGeneration','-StreamlineRuntimeDirectory',$streamline.ToUpperInvariant()); Width='2560'; Height='1440'; Preset=$false; NativeFG=$true; Valid=$true},
        @{Name='native-fg-split-runtime-rejected'; Args=@('-NativePssr','-NativeFrameGeneration','-StreamlineRuntimeDirectory',$separateStreamline); Error='same directory'; Valid=$false},
        @{Name='legacy-fg-split-runtime-unchanged'; Args=@('-NativeFrameGeneration','-StreamlineRuntimeDirectory',$separateStreamline); Width='2560'; Height='1440'; Preset=$false; Native=$false; NativeFG=$true; Valid=$true},
        @{Name='nvidia-fg-missing-runtime'; Args=@('-NativePssr','-NativePssr1080To4K','-NativeFrameGeneration'); Error='requires -StreamlineRuntimeDirectory'; Valid=$false},
        @{Name='native-two-fg-providers'; Args=@('-NativePssr','-NativeFrameGeneration','-FrameGeneration','-StreamlineRuntimeDirectory',$streamline); Valid=$false},
        @{Name='native-fg-input-capture-conflict'; Args=@('-NativePssr','-NativeFrameGeneration','-CaptureTemporalInputs','-StreamlineRuntimeDirectory',$streamline); Valid=$false},
        @{Name='native-fg-output-capture-conflict'; Args=@('-NativePssr','-NativeFrameGeneration','-CaptureGeneratedFrames','-StreamlineRuntimeDirectory',$streamline); Valid=$false},
        @{Name='native-fg-occlusion-guard-conflict'; Args=@('-NativePssr','-NativeFrameGeneration','-OcclusionGuard','-StreamlineRuntimeDirectory',$streamline); Valid=$false},
        @{Name='native-async-requires-fg'; Args=@('-NativePssr','-AsyncNativePresentation'); Valid=$false},
        @{Name='native-sr-manifest'; Args=@('-NativePssr','-NativePssr1080To4K','-DlssPreset','M'); Width='3840'; Height='2160'; Preset=$true; StubRun=$true; Valid=$true},
        @{Name='native-fg-2x-manifest'; Args=@('-NativePssr','-NativePssr1080To4K','-NativeFrameGeneration','-StreamlineRuntimeDirectory',$streamline,'-DlssPreset','M'); Width='3840'; Height='2160'; Preset=$true; NativeFG=$true; StubRun=$true; Valid=$true}
    )
    foreach ($missing in $streamlineFiles) {
        $incomplete = Join-Path $root ('missing-' + $missing)
        [void][IO.Directory]::CreateDirectory($incomplete)
        Copy-Item -LiteralPath (Join-Path $fixture 'nvngx_dlss.dll') -Destination (Join-Path $incomplete 'nvngx_dlss.dll')
        foreach ($name in $streamlineFiles) {
            if ($name -cne $missing) { [IO.File]::WriteAllText((Join-Path $incomplete $name), 'Offline fixture only.') }
        }
        $cases += @{Name=('dependency-' + $missing); Runtime=$incomplete; Args=@('-NativePssr','-NativeFrameGeneration','-StreamlineRuntimeDirectory',$incomplete); Error=('Missing native frame generation runtime: ' + [regex]::Escape($missing)); Valid=$false}
    }
    foreach ($case in $cases) {
        $outputRoot = Join-Path $root ('must-not-create-' + $case.Name)
        $record = Join-Path $root ($case.Name + '.arguments.txt')
        $environmentRecord = Join-Path $root ($case.Name + '.environment.txt')
        $stubRun = $case.ContainsKey('StubRun') -and $case.StubRun
        $nativeFg = $case.ContainsKey('NativeFG') -and $case.NativeFG
        $isNative = -not $case.ContainsKey('Native') -or $case.Native
        $caseRuntime = $(if ($case.ContainsKey('Runtime')) { $case.Runtime } else { $fixture })
        $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $scripts 'GT7-Test-DLSS.ps1'),
            '-GamePath',$fixture,'-EmulatorPath',$probe,'-BackendPath',(Join-Path $fixture 'backend.dll'),
            '-RuntimeDirectory',$caseRuntime,'-SourceUserDataRoot',$seed,'-OutputRoot',$outputRoot) + $case.Args
        if (-not $stubRun) { $arguments += '-CheckOnly' }
        $child = New-Object System.Diagnostics.Process
        try {
            $child.StartInfo = New-GT7ProcessStartInfo -Executable $PowerShellExecutable -ArgumentList $arguments -WorkingDirectory $root -CaptureOutput
            $child.StartInfo.EnvironmentVariables['NATIVE_PSSR_TEST_ARGUMENT_LOG'] = $record
            $child.StartInfo.EnvironmentVariables['NATIVE_PSSR_TEST_ENVIRONMENT_LOG'] = $environmentRecord
            $child.StartInfo.EnvironmentVariables['NATIVE_PSSR_TEST_ALLOW_STUB_RUN'] = $(if ($stubRun) { '1' } else { '0' })
            # The real launch must clear inherited conflicting KYTY settings.
            $child.StartInfo.EnvironmentVariables['KYTY_GT7_DLSS_FG'] = '9'
            $child.StartInfo.EnvironmentVariables['KYTY_GT7_FSR_FG'] = '1'
            $child.StartInfo.EnvironmentVariables['KYTY_GT7_DLSS_FG_RUNTIME'] = 'must-not-inherit'
            [void]$child.Start()
            $stdout = $child.StandardOutput.ReadToEndAsync()
            $stderr = $child.StandardError.ReadToEndAsync()
            $child.WaitForExit()
            $text = $stdout.GetAwaiter().GetResult()
            $errorText = $stderr.GetAwaiter().GetResult()
            [IO.File]::WriteAllText((Join-Path $root ($case.Name + '.stdout.txt')), $text)
            [IO.File]::WriteAllText((Join-Path $root ($case.Name + '.stderr.txt')), $errorText)
            if (($child.ExitCode -eq 0) -ne $case.Valid) { throw ($case.Name + ': unexpected exit ' + $child.ExitCode + ' ' + $errorText) }
            if (-not $stubRun -and (Test-Path -LiteralPath $outputRoot)) { throw ($case.Name + ': created OutputRoot') }
            $seedAfter = SeedSnapshot
            if ($seedAfter -cne $seedBefore) {
                [IO.File]::WriteAllText((Join-Path $root 'seed-after.txt'), $seedAfter)
                throw ($case.Name + ': modified the seed')
            }
            if ($case.Valid) {
                $lines = @(Get-Content -LiteralPath $record)
                $expectedCalls = $(if ($stubRun) { 2 } else { 1 })
                if (@($lines | Where-Object { $_ -ceq 'CALL' }).Count -ne $expectedCalls) { throw ($case.Name + ': wrong preflight/stub invocation count') }
                $argv = @($lines | Where-Object { $_ -cne 'CALL' } | ForEach-Object { [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($_)) })
                foreach ($flag in @('--gt7-check-only','--ps5-pro','--pssr-dlss')) {
                    $expectedFlagCount = $(if ($flag -ceq '--gt7-check-only') { 1 } elseif ($isNative) { $expectedCalls } else { 0 })
                    if (@($argv | Where-Object { $_ -ceq $flag }).Count -ne $expectedFlagCount) { throw ($case.Name + ': missing/duplicate ' + $flag) }
                }
                $desktopHeight = $(if ($isNative) { 'native' } else { '1080' })
                foreach ($pair in @(@('--screen-width',$case.Width),@('--screen-height',$case.Height),@('--gt7-desktop-height',$desktopHeight))) {
                    $index = [Array]::IndexOf($argv, $pair[0])
                    if ($index -lt 0 -or $index + 1 -ge $argv.Count -or $argv[$index + 1] -cne $pair[1]) { throw ($case.Name + ': wrong ' + $pair[0]) }
                }
                if (($argv -ccontains '--gt7-pssr-1080-4k') -ne $case.Preset -or $argv -contains '--graphics-check-only') { throw ($case.Name + ': wrong preset/check flag') }
                if (-not $stubRun -and $text -notmatch 'Playtest preflight passed') { throw ($case.Name + ': missing preflight success') }
                if ($stubRun) {
                    $manifests = @(Get-ChildItem -LiteralPath $outputRoot -Filter 'playtest-manifest.json' -Recurse -File)
                    if ($manifests.Count -ne 1) { throw ($case.Name + ': expected one fixture manifest') }
                    $manifest = Get-Content -LiteralPath $manifests[0].FullName -Raw | ConvertFrom-Json
                    if ($manifest.status -cne 'finished' -or $manifest.exit_code -ne 0 -or
                        -not $manifest.pssr_dlss_requested -or -not $manifest.native_pssr_1080_to_4k_requested -or
                        $manifest.frame_generation_requested -ne $nativeFg -or $manifest.frame_generation_verified -or
                        $manifest.dlss_preset_requested -cne 'M' -or $manifest.super_resolution_input_source -cne 'native-psml-pssr') {
                        throw ($case.Name + ': wrong native PSSR/FG manifest contract')
                    }
                    $expectedMultiplier = $(if ($nativeFg) { 2 } else { 1 })
                    $expectedGenerated = $(if ($nativeFg) { 1 } else { 0 })
                    $expectedProvider = $(if ($nativeFg) { 'nvidia-streamline-vulkan' } else { 'none' })
                    if ($manifest.frame_generation_requested_multiplier -ne $expectedMultiplier -or
                        $manifest.frame_generation_requested_generated_frames_per_real -ne $expectedGenerated -or
                        $manifest.frame_generation_provider -cne $expectedProvider -or
                        $manifest.native_pssr_target.verified -or $manifest.native_pssr_target.input_width -ne 1920 -or
                        $manifest.native_pssr_target.output_width -ne 3840 -or $manifest.window_requested.height -ne 2160) {
                        throw ($case.Name + ': wrong fixed-2X or resolution request')
                    }
                    $environment = @{}
                    foreach ($line in Get-Content -LiteralPath $environmentRecord) {
                        $pair = $line.Split([char[]]'=', 2)
                        $environment[$pair[0]] = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($pair[1]))
                    }
                    $expectedFg = $(if ($nativeFg) { '1' } else { '0' })
                    foreach ($pair in @(@('KYTY_GT7_DLSS','0'),@('KYTY_GT7_FSR_FG','0'),@('KYTY_GT7_DLSS_FG',$expectedFg),@('KYTY_GT7_DLSS_PRESET','M'),@('KYTY_PSSR_DIAGNOSTICS','0'),@('KYTY_GT7_NATIVE_ASYNC_PRESENT','0'))) {
                        if ($environment[$pair[0]] -cne $pair[1] -or $manifest.environment.($pair[0]) -cne $pair[1]) { throw ($case.Name + ': wrong environment ' + $pair[0]) }
                    }
                    if ($nativeFg) {
                        if ($environment['KYTY_GT7_DLSS_FG_RUNTIME'] -cne $streamline -or
                            $environment['DISABLE_RTSS_LAYER'] -cne '1' -or $manifest.streamline_runtime_directory -cne $streamline -or
                            @($manifest.streamline_runtime_files).Count -ne 7 -or $manifest.note -notmatch 'Fixed 2X NVIDIA' -or
                            $manifest.note -match 'Frame generation remains disabled') { throw ($case.Name + ': wrong native FG runtime/note') }
                        foreach ($file in $manifest.streamline_runtime_files) {
                            if ($streamlineFiles -cnotcontains $file.name -or $file.sha256 -cne (Get-FileHash -LiteralPath (Join-Path $streamline $file.name) -Algorithm SHA256).Hash.ToLowerInvariant()) { throw ($case.Name + ': wrong runtime file hash') }
                        }
                    } elseif ($environment['KYTY_GT7_DLSS_FG_RUNTIME'] -cne '' -or $manifest.note -notmatch 'Frame generation remains disabled') {
                        throw ($case.Name + ': ordinary SR-only launch inherited native FG')
                    }
                }
            } elseif (Test-Path -LiteralPath $record) { throw ($case.Name + ': invalid arguments reached the emulator stub') }
            if ($case.ContainsKey('Error') -and $errorText -notmatch $case.Error) { throw ($case.Name + ': wrong rejection reason') }
            $result.checks += [ordered]@{ name=$case.Name; exit_code=$child.ExitCode; passed=$true }
        } finally { $child.Dispose() }
    }
    $result.passed = $true
} catch {
    $result.error = $_.Exception.Message
    throw
} finally {
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'result.json') -Encoding UTF8
    Write-Output ('Native PSSR launcher test result: ' + (Join-Path $root 'result.json'))
}
