#requires -Version 5.1
<#!
Local copy-only candidate builder. This is deliberately separate from the public
1.0.4 packager: no archive, publishing, process launch, or user-data migration.
The frozen FG manifest pins its entire payload, including corresponding source.
!#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$InstallDirectory,
    [Parameter(Mandatory=$true)][string]$FrameGenerationDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$fgExeHash = '9b9442d8791f6c36385ffd7282e64ace40cbb65f0cb77bfffc29e01aa76ee92e'
$fgManifestHash = 'a92bf197ddda0d67845f083f8de07e9e313eb6623233ea9f0dad9903ae5cd78e'
$fgNoticeHash = '6e922e8a6d77f544294ddfceef7b65ab34e80c373d9aa2655ff243d4a801275d'

function Assert-NoLinkAncestors([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrEmpty($current)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Links are not accepted in package paths: $current"
            }
        }
        $current = [IO.Path]::GetDirectoryName($current)
    }
}
function Get-Root([string]$Path) {
    Assert-NoLinkAncestors $Path
    $item = Get-Item -LiteralPath $Path -Force
    if (-not $item.PSIsContainer) { throw "Expected directory: $Path" }
    return $item.FullName.TrimEnd('\','/')
}
function Get-SafeChild([string]$Root, [string]$Relative) {
    if ([string]::IsNullOrWhiteSpace($Relative) -or $Relative.Contains('\') -or
        $Relative -match '[:*?"<>|\x00-\x1f]' -or [IO.Path]::IsPathRooted($Relative)) {
        throw "Unsafe package-relative path: $Relative"
    }
    foreach ($part in $Relative.Split('/')) {
        if ($part -in @('', '.', '..') -or $part.TrimEnd(' ','.') -cne $part -or
            $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw "Unsafe package path component: $Relative"
        }
    }
    $result = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (-not $result.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package path escaped its input directory: $Relative"
    }
    return $result
}
function Get-RegularFiles([string]$Directory) {
    # Enumerate only explicitly selected trees; never follow links into user data.
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($Directory)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($item in Get-ChildItem -LiteralPath $current -Force) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Linked content is forbidden: $($item.FullName)"
            }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) } else { $item }
        }
    }
}
function Assert-CleanPayload([string]$Relative, [switch]$FrameGeneration) {
    $extension = [IO.Path]::GetExtension($Relative)
    if ($Relative -match '(^|/)(eboot\.bin|gt7save\.bin|_SaveData|_DownloadData|_Trophies|user[-_]?data|saves?|caches?|shader[-_]cache|_PipelineCache|captures?|logs?|outputs|include|lib)(/|$)' -or
        $extension -in @('.pdb','.map','.log','.dmp','.gpudmp','.bin','.raw','.rgba16f')) {
        throw "Game, user state, build or diagnostic content is forbidden: $Relative"
    }
    if ($extension -ieq '.ini' -and (-not $FrameGeneration -or
        $Relative -notin @('runtimes/dlssg_sm86/native.ini','runtimes/dlssg_sm86/forward.ini'))) {
        throw "Unexpected configuration file: $Relative"
    }
    if (-not $FrameGeneration -and ($Relative -match '(^|/)(sl\.[^/]*\.dll|nvngx_dlssg\.dll|NvLowLatencyVk\.dll|amd_fidelityfx_vk\.dll|Magpie\.exe|TohiFrameGen\.exe)$' -or
        $Relative -match '^licenses/(nvidia/streamline|fidelityfx)/' -or
        $Relative -match '(^|/)NVIDIA-STREAMLINE-NOTICE\.md$')) {
        throw "Old in-process FG components are forbidden: $Relative"
    }
}
$plan = New-Object 'System.Collections.Generic.List[object]'
$planned = @{}
function Add-Payload([string]$Root, [string]$Relative, [string]$Destination,
    [string]$ExpectedHash = '', [long]$ExpectedBytes = -1, [switch]$FrameGeneration) {
    Assert-CleanPayload $Relative -FrameGeneration:$FrameGeneration
    $path = Get-SafeChild $Root $Relative
    Assert-NoLinkAncestors $path
    $file = Get-Item -LiteralPath $path -Force
    if ($file.PSIsContainer) { throw "Expected regular file: $Relative" }
    if ($planned.ContainsKey($Destination)) { throw "Duplicate package path: $Destination" }
    if ($ExpectedBytes -ge 0 -and $file.Length -ne $ExpectedBytes) { throw "Size mismatch: $Relative" }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($ExpectedHash -and ($ExpectedHash -notmatch '^[0-9a-fA-F]{64}$' -or $hash -ine $ExpectedHash)) {
        throw "SHA256 mismatch: $Relative"
    }
    $record = [pscustomobject]@{ path=$Destination; source=$path; bytes=$file.Length; sha256=$hash }
    $plan.Add($record)
    $planned[$Destination] = $record
}

# Refuse existing outputs before reading any source payload. Never delete an old run.
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
if (Test-Path -LiteralPath $output) { throw 'OutputDirectory already exists; choose a new directory.' }
Assert-NoLinkAncestors $output
$install = Get-Root $InstallDirectory
$fgRoot = Get-Root $FrameGenerationDirectory
foreach ($inputRoot in @($install,$fgRoot)) {
    if ($output.Equals($inputRoot,[StringComparison]::OrdinalIgnoreCase) -or
        $output.StartsWith($inputRoot + '\',[StringComparison]::OrdinalIgnoreCase) -or
        $inputRoot.StartsWith($output + '\',[StringComparison]::OrdinalIgnoreCase)) {
        throw 'OutputDirectory must be separate from both input trees.'
    }
}

# Pin both generated FG metadata files as well as every file described by them.
Add-Payload $fgRoot 'fg-runtime-manifest.json' 'framegen/fg-runtime-manifest.json' $fgManifestHash -FrameGeneration
Add-Payload $fgRoot 'FG-COMPONENT-NOTICE.txt' 'framegen/FG-COMPONENT-NOTICE.txt' $fgNoticeHash -FrameGeneration
$fg = Get-Content -LiteralPath (Join-Path $fgRoot 'fg-runtime-manifest.json') -Raw | ConvertFrom-Json
if ($fg.schema -ne 1 -or $fg.version -cne '1.0.5' -or $fg.candidate_only -ne $true -or
    $fg.public_release_ready -ne $false -or $fg.dependency_notices_supplied -ne $true -or
    $fg.corresponding_source_artifacts_supplied -ne $true -or $fg.executable_sha256 -cne $fgExeHash) {
    throw 'FG stage does not match the frozen local 1.0.5 contract.'
}
if ($fg.runtime_policy.capture -cne 'WGC' -or $fg.runtime_policy.multiplier -ne 2 -or
    $fg.runtime_policy.opticalFlowMethod -ne 0 -or $fg.runtime_policy.upscale -cne 'none' -or
    $fg.runtime_policy.input_equals_output_dimensions -ne $true -or
    $fg.featureOptions.EnableDLSSFrameGeneration -ne $true) { throw 'Unexpected FG runtime policy.' }
foreach ($name in @('EnableDLSSZeroMV','EnableDLSSSR','EnableDLSSNR','EnableNvidiaOpticalFlow',
    'EnableFSR2ZeroMV','EnableFSR3ZeroMV','EnableFSR3FrameGeneration','EnableAmdOpticalFlow',
    'EnableXeSSZeroMV','EnableXeSSFrameGeneration','EnableRTXVideoDenoise','EnableNvidiaVfx',
    'EnableNvidiaVfxAsync','EnableNvidiaVfxCudaHandoff','EnableFrameTrace','EnableNativeBackendTiming',
    'DebugBorder','DebugInfoOnOverlay','UseCompSwapchain')) {
    if ($fg.featureOptions.$name -ne $false) { throw "Unexpected enabled FG build feature: $name" }
}
$fgExpected = @{'fg-runtime-manifest.json'=$true; 'FG-COMPONENT-NOTICE.txt'=$true}
foreach ($entry in $fg.files) {
    $relative = [string]$entry.path
    if ($fgExpected.ContainsKey($relative)) { throw "Duplicate FG manifest path: $relative" }
    Add-Payload $fgRoot $relative ('framegen/' + $relative) ([string]$entry.sha256) ([long]$entry.bytes) -FrameGeneration
    $fgExpected[$relative] = $true
}
if ($fg.files.Count -ne 31 -or $planned['framegen/TohiFrameGen.exe'].sha256 -cne $fgExeHash) {
    throw 'Frozen FG payload count or executable changed.'
}
$fgActual = @(Get-RegularFiles $fgRoot)
if ($fgActual.Count -ne $fgExpected.Count) { throw 'FG stage contains missing or extra files.' }
foreach ($item in $fgActual) {
    $relative = $item.FullName.Substring($fgRoot.Length + 1).Replace('\','/')
    if (-not $fgExpected.ContainsKey($relative)) { throw "Unlisted FG stage file: $relative" }
}

$required = @('launcher.exe','kyty_emulator.exe','Start-GT7-Desktop.ps1','Start-GT7-Desktop.cmd','LICENSE',
    'Qt6Core.dll','Qt6Gui.dll','Qt6Network.dll','Qt6Svg.dll','Qt6Widgets.dll','libwinpthread-1.dll',
    'msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll','msvcp140_1.dll','msvcp140_2.dll',
    'msvcp140_atomic_wait.dll','msvcp140_codecvt_ids.dll','icuuc.dll','platforms/qwindows.dll',
    'kyty_dlss_backend.dll','nvngx_dlss.dll','translations/qt_zh_CN.qm',
    'docs/GT7.md','docs/NATIVE-PSSR-DLSS.md','docs/RELEASE-1.0.5.zh-CN.md',
    'licenses/Kyty-MIT.txt','licenses/nvidia/LICENSE.txt','licenses/nvidia/NVIDIA-DLSS-NOTICE.md',
    'licenses/ffmpeg/copyright','licenses/ffmpeg/SOURCE.txt','licenses/opus/COPYING',
    'licenses/qt/Qt-NOTICE.md','licenses/qt/Qt-LGPL-3.0-only.txt','licenses/qt/Qt-GPL-3.0-only.txt',
    'licenses/openvr/LICENSE','fonts/NotoSansCJKsc-Regular.otf','licenses/fonts/noto-cjk/OFL.txt',
    'licenses/fonts/noto-cjk/NOTICE.md','licenses/fonts/noto-cjk/provenance.json','licenses/winpthread/COPYING',
    'patches/GT7/patches.json')
$installPins = @{
    'kyty_dlss_backend.dll'='60cfdbbc405a641570e30c330638e5e662a271f977638bdd2df36f1d80f06ecf'
    'nvngx_dlss.dll'='d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e'
    'licenses/nvidia/LICENSE.txt'='d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0'
    'patches/GT7/GT7-PSSR-1440-to-4K.json'='5b05b13d0c9732fb84781b3e20b3cac2865a090ad73d49b0b1c0eb4602b94a1d'
    'fonts/NotoSansCJKsc-Regular.otf'='2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b'
    'licenses/fonts/noto-cjk/OFL.txt'='6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2'
}
foreach ($relative in @('nvngx_dlssg.dll','NvLowLatencyVk.dll','sl.interposer.dll','sl.common.dll',
    'sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll','amd_fidelityfx_vk.dll','framegen',
    'licenses/nvidia/NVIDIA-STREAMLINE-NOTICE.md','licenses/nvidia/streamline','licenses/fidelityfx')) {
    if (Test-Path -LiteralPath (Join-Path $install $relative)) { throw "Stale internal FG content: $relative" }
}
foreach ($relative in $required) { Add-Payload $install $relative $relative $installPins[$relative] }
foreach ($item in Get-ChildItem -LiteralPath $install -Filter '*.dll' -File -Force) {
    if ($item.Name -notmatch '^(Qt6[A-Za-z0-9_]+|icu[a-z0-9_]*|concrt140|msvcp140(?:_[a-z0-9_]+)?|vcruntime140(?:_[a-z0-9_]+)?|vccorlib140|libwinpthread-1|kyty_dlss_backend|nvngx_dlss)\.dll$') {
        throw "Unexpected installation DLL: $($item.Name)"
    }
    if (-not $planned.ContainsKey($item.Name)) { Add-Payload $install $item.Name $item.Name }
}
foreach ($directory in @('assets','generic','iconengines','imageformats','networkinformation','platforms','styles','tls','translations')) {
    $path = Join-Path $install $directory
    if (-not (Test-Path -LiteralPath $path)) { continue }
    Assert-NoLinkAncestors $path
    foreach ($item in Get-RegularFiles $path) {
        $relative = $item.FullName.Substring($install.Length + 1).Replace('\','/')
        if ($directory -eq 'assets') {
            if ($item.Extension -notin @('.wav','.png','.jpg','.jpeg','.svg','.ico')) { throw "Unexpected asset: $relative" }
        } elseif ($directory -eq 'translations') {
            if ($item.Extension -ine '.qm') { throw "Unexpected translation: $relative" }
        } elseif ($item.Extension -ine '.dll') { throw "Unexpected runtime plugin: $relative" }
        if (-not $planned.ContainsKey($relative)) { Add-Payload $install $relative $relative }
    }
}
$patches = Get-Content -LiteralPath (Join-Path $install 'patches/GT7/patches.json') -Raw | ConvertFrom-Json
$native = @($patches.patches | Where-Object { $_.file -ceq 'GT7-PSSR-1440-to-4K.json' })
if ($native.Count -ne 1 -or $native[0].sha256 -cne $installPins['patches/GT7/GT7-PSSR-1440-to-4K.json']) {
    throw 'Validated native 1440p-to-4K profile is not registered exactly once.'
}
foreach ($patch in $patches.patches) {
    if ([string]$patch.file -match '[/\\:]|^\.\.?$' -or [string]::IsNullOrWhiteSpace([string]$patch.file)) {
        throw 'GT7 patch manifest has an unsafe filename.'
    }
    $relative = 'patches/GT7/' + $patch.file
    Add-Payload $install $relative $relative ([string]$patch.sha256)
}
$srPath = Join-Path $install 'nvngx_dlss.dll'
$signature = Get-AuthenticodeSignature -LiteralPath $srPath
if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
    $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=NVIDIA Corporation(,|$)') {
    throw 'SR runtime lacks a valid NVIDIA signature.'
}
$srVersion = (Get-Item -LiteralPath $srPath).VersionInfo
if (('{0}.{1}.{2}.{3}' -f $srVersion.FileMajorPart,$srVersion.FileMinorPart,$srVersion.FileBuildPart,$srVersion.FilePrivatePart) -cne '310.9.16.0') {
    throw 'SR runtime version is not the validated 310.9.16.0.'
}

# All validation above is read-only. Copy only the explicit plan, then rehash every
# destination. A copy failure leaves an incomplete new directory, never an old one.
if (Test-Path -LiteralPath $output) { throw 'Output appeared during validation; refusing to overwrite.' }
[void][IO.Directory]::CreateDirectory($output)
$outputRecords = New-Object 'System.Collections.Generic.List[object]'
foreach ($record in ($plan | Sort-Object path)) {
    $destination = Get-SafeChild $output $record.path
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
    [IO.File]::Copy($record.source,$destination,$false)
    $copied = Get-Item -LiteralPath $destination
    $hash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($copied.Length -ne $record.bytes -or $hash -cne $record.sha256) { throw "Copied payload mismatch: $($record.path)" }
    $outputRecords.Add([pscustomobject]@{path=$record.path;bytes=$copied.Length;sha256=$hash})
}
$notice = @'
Tohi-GT7 1.0.5 - LOCAL CANDIDATE ONLY

This directory was copied from the explicit emulator installation and frozen
headless frame-generation stage. It contains no game, saves, caches, run logs,
debug symbols, or build libraries. No application was launched by packaging.

Game reconstruction: 2560x1440 -> 3840x2160 native PSSR inputs through DLSS SR.
Optional separate capture host: WGC, same resolution, DLSSFG 2X, Auto preset.
Internal NVIDIA/FSR frame generation is not packaged. No extra FPS cap is added.
Supported candidate GPU families: GeForce RTX 40/50; no all-model claim is made.

This is not a public release or a claim of completed game-performance testing.
Read docs/RELEASE-1.0.5.zh-CN.md and framegen/FG-COMPONENT-NOTICE.txt.
FG licenses and corresponding source materials are retained without rewriting.
The upstream stage manifest retains its original unresolved review notes; this
copy operation does not independently resolve those notes or redistribution.

SHA256SUMS.txt covers every file except itself. Adding personal data or replacing
binaries changes this clean candidate and requires a fresh integrity record.
'@
[IO.File]::WriteAllText((Join-Path $output 'LOCAL-CANDIDATE.txt'),$notice + "`n",$utf8)
$manifest = [ordered]@{
    schema=1; product='Tohi-GT7'; version='1.0.5'; candidate_only=$true; public_release_ready=$false
    created_utc=[DateTime]::UtcNow.ToString('o'); user_data_included=$false; game_included=$false
    internal_frame_generation_packaged=$false; external_frame_generation='WGC DLSSFG 2X Auto'
    framegen_executable_sha256=$fgExeHash; framegen_stage_manifest_sha256=$fgManifestHash
    reconstruction_input=@(2560,1440); reconstruction_output=@(3840,2160); extra_fps_cap=0
    validation='Input policy and SHA256 verified; every copied payload rehashed. No game or GPU test launched.'
    payload_files=@($outputRecords.ToArray())
}
[IO.File]::WriteAllText((Join-Path $output 'local-candidate-manifest.json'),($manifest | ConvertTo-Json -Depth 8) + "`n",$utf8)
foreach ($relative in @('LOCAL-CANDIDATE.txt','local-candidate-manifest.json')) {
    $path = Join-Path $output $relative
    $outputRecords.Add([pscustomobject]@{path=$relative;bytes=(Get-Item -LiteralPath $path).Length;
        sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()})
}
$sums = @($outputRecords | Sort-Object path | ForEach-Object { $_.sha256 + '  ' + $_.path })
[IO.File]::WriteAllLines((Join-Path $output 'SHA256SUMS.txt'),[string[]]$sums,$utf8)
[pscustomobject]@{output=$output;version='1.0.5';candidate_only=$true;files=$outputRecords.Count + 1;
    payload_files=$plan.Count;framegen_payload_files=$fg.files.Count;framegen_sha256=$fgExeHash;
    sums_sha256=(Get-FileHash -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Algorithm SHA256).Hash.ToLowerInvariant()}
