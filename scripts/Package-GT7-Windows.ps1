[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$InstallDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)]
    [ValidatePattern('^v?\d+\.\d+\.\d+$')][string]$ReleaseTag
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = (Resolve-Path -LiteralPath $InstallDirectory).ProviderPath
$output = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($output)
$version = $ReleaseTag -replace '^v', ''
$name = 'Tohi-GT7-' + $version + '-Windows-x64'
$stage = Join-Path $output $name
$archive = Join-Path $output ($name + '.zip')
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $archive)) {
    throw 'Release stage or archive already exists. Use a new output directory or release tag.'
}

$required = @('launcher.exe','kyty_emulator.exe','Start-GT7-Desktop.ps1','Start-GT7-Desktop.cmd','Qt6Core.dll','Qt6Gui.dll',
    'Qt6Network.dll','Qt6Svg.dll','Qt6Widgets.dll','libwinpthread-1.dll','msvcp140.dll',
    'vcruntime140.dll','vcruntime140_1.dll','msvcp140_1.dll','msvcp140_2.dll',
    'msvcp140_atomic_wait.dll','msvcp140_codecvt_ids.dll','icuuc.dll',
    'platforms\qwindows.dll','patches\GT7\patches.json','patches\GT7\GT7-PSSR-1080-to-4K.json',
    'kyty_dlss_backend.dll','nvngx_dlss.dll','nvngx_dlssg.dll','NvLowLatencyVk.dll',
    'sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll',
    'docs\GT7.md','docs\NATIVE-PSSR-DLSS.md','docs\GT7-NVIDIA-FRAME-GENERATION.md',("docs\RELEASE-" + $version + '.zh-CN.md'),'LICENSE',
    'translations\qt_zh_CN.qm',
    'licenses\Kyty-MIT.txt','licenses\nvidia\LICENSE.txt',
    'licenses\nvidia\NVIDIA-DLSS-NOTICE.md',
    'licenses\nvidia\NVIDIA-STREAMLINE-NOTICE.md',
    'licenses\nvidia\streamline\LICENSE.txt','licenses\nvidia\streamline\THIRD-PARTY-LICENSES.md',
    'licenses\nvidia\streamline\RTX-SDK-LICENSE.txt','licenses\nvidia\streamline\REFLEX-LICENSE.txt',
    'licenses\ffmpeg\copyright','licenses\ffmpeg\SOURCE.txt','licenses\opus\COPYING',
    'licenses\qt\Qt-NOTICE.md','licenses\qt\Qt-LGPL-3.0-only.txt',
    'licenses\qt\Qt-GPL-3.0-only.txt','licenses\openvr\LICENSE',
    'fonts\NotoSansCJKsc-Regular.otf','licenses\fonts\noto-cjk\OFL.txt',
    'licenses\fonts\noto-cjk\NOTICE.md','licenses\fonts\noto-cjk\provenance.json',
    'licenses\winpthread\COPYING')
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $relative) -PathType Leaf)) {
        throw "Windows installation is incomplete: $relative"
    }
}

# Pin the exact PSSR backend and production SR/FG bundle used by the native
# integration. All eight NVIDIA DLLs must share the extracted release directory.
$backendHash = '60cfdbbc405a641570e30c330638e5e662a271f977638bdd2df36f1d80f06ecf'
if ((Get-FileHash -LiteralPath (Join-Path $source 'kyty_dlss_backend.dll') -Algorithm SHA256).Hash -ine $backendHash) {
    throw 'Unvalidated or stale PSSR backend in Windows installation.'
}
$vendorHashes = [ordered]@{
    'nvngx_dlss.dll' = 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e'
    'sl.interposer.dll' = 'b1d7395aebd7b9722841d64dcf7433680020b6a8e421f851c16d5ba0c2182edf'
    'sl.common.dll' = '38eea3b4209afc455636ea372b5933d65fcc13becec6c84d101dcfc4d8648f75'
    'sl.dlss_g.dll' = '3ed9538df724993bea8a050195a6a78b9c8c8a566d5b05ade477aab3c390e90f'
    'sl.reflex.dll' = '443577e869352436ff7ec8f4d9c90418d3e95368436ce219261e0f4f07a38122'
    'sl.pcl.dll' = '5698c38e722318681f215c9fe68e45b9bedd51378acc699d8eee26e961e57770'
    'nvngx_dlssg.dll' = '8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7'
    'NvLowLatencyVk.dll' = '2a77dc3e1c724b7eea5755be0ae7423752e79a2459fae72181a9f00e3507e5d6'
}
$vendorEvidence = @($vendorHashes.Keys | ForEach-Object {
    $relative = $_
    $path = Join-Path $source $relative
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $vendorHashes[$relative]) { throw "Unvalidated vendor runtime: $relative" }
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=NVIDIA Corporation(,|$)') {
        throw "Vendor runtime lacks a valid NVIDIA signature: $relative"
    }
    $file = Get-Item -LiteralPath $path
    $version = $null
    if (-not [string]::IsNullOrWhiteSpace($file.VersionInfo.FileVersion)) {
        $version = '{0}.{1}.{2}.{3}' -f $file.VersionInfo.FileMajorPart, $file.VersionInfo.FileMinorPart,
            $file.VersionInfo.FileBuildPart, $file.VersionInfo.FilePrivatePart
    }
    $expectedVersion = $(if ($relative -eq 'NvLowLatencyVk.dll') { $null }
        elseif ($relative.StartsWith('sl.')) { '2.14.16.0' } else { '310.9.16.0' })
    if ($version -ne $expectedVersion) { throw "Vendor runtime version mismatch: $relative" }
    [ordered]@{name=$relative; sha256=$actualHash; version=$version;
        signature=$signature.Status.ToString(); signer=$signature.SignerCertificate.Subject}
})
# A build may retain the legacy FSR feature; it is not the native preset provider.
if (Test-Path -LiteralPath (Join-Path $source 'amd_fidelityfx_vk.dll') -PathType Leaf) {
    $vendorHashes['amd_fidelityfx_vk.dll'] = 'a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78'
    if ((Get-FileHash -LiteralPath (Join-Path $source 'amd_fidelityfx_vk.dll') -Algorithm SHA256).Hash -ine $vendorHashes['amd_fidelityfx_vk.dll']) {
        throw 'Unvalidated optional legacy FSR runtime.'
    }
    foreach ($relative in @('licenses\fidelityfx\LICENSE.txt','licenses\fidelityfx\provenance.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $source $relative) -PathType Leaf)) {
            throw "Missing optional FSR runtime notice: $relative"
        }
    }
}
$licenseHashes = [ordered]@{
    'licenses\nvidia\streamline\LICENSE.txt' = '7b6f23e7d6f3ad6292f9308d2b42cdc3d82ae4e9b2abb55f230279d83bedd43d'
    'licenses\nvidia\streamline\THIRD-PARTY-LICENSES.md' = 'cb251639994465f31d2178c16acedf1ff4f9cef1c370a09782ed36d4c86c1dca'
    'licenses\nvidia\streamline\RTX-SDK-LICENSE.txt' = '3027f23ca5a46dd9cb8183fbd522983a86f64d7daac5982912bf9f214671f294'
    'licenses\nvidia\streamline\REFLEX-LICENSE.txt' = 'ebf83c07fb3b2939908c3795d887afde3161c89a28ba391724efc784ce1bdabe'
}
foreach ($relative in $licenseHashes.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $source $relative) -Algorithm SHA256).Hash -ine $licenseHashes[$relative]) {
        throw "Changed or missing vendor license: $relative"
    }
}
$dlssVersion = (Get-Item -LiteralPath (Join-Path $source 'nvngx_dlss.dll')).VersionInfo.FileVersion

if ((Get-FileHash -LiteralPath (Join-Path $source 'patches\GT7\GT7-PSSR-1080-to-4K.json') -Algorithm SHA256).Hash -ine
    '282da91e431164d1c2437bf7071239e2b26ec1cd824bf0a86e938bb6fce32b30') {
    throw 'Unvalidated native 1080p-to-4K profile.'
}

$patchManifest = Get-Content -LiteralPath (Join-Path $source 'patches\GT7\patches.json') -Raw | ConvertFrom-Json
$nativePatch = @($patchManifest.patches | Where-Object { $_.file -ceq 'GT7-PSSR-1080-to-4K.json' })
if ($nativePatch.Count -ne 1 -or $nativePatch[0].sha256 -cne
    '282da91e431164d1c2437bf7071239e2b26ec1cd824bf0a86e938bb6fce32b30') {
    throw 'Native 1080p-to-4K profile is not registered with its validated digest.'
}
foreach ($patch in $patchManifest.patches) {
    if ([string]$patch.file -match '[/\\:]|^\.\.?$' -or [string]::IsNullOrWhiteSpace([string]$patch.file)) {
        throw 'GT7 patch manifest contains an unsafe filename.'
    }
    if ((Get-FileHash -LiteralPath (Join-Path $source ('patches\GT7\' + $patch.file)) -Algorithm SHA256).Hash -ine $patch.sha256) {
        throw "GT7 patch manifest hash mismatch: $($patch.file)"
    }
}

# Only the pinned, unmodified OFL font is distributable through this package.
# Never enumerate/copy a user's fonts folder or the Windows Fonts directory.
$fontHashes = [ordered]@{
    'fonts\NotoSansCJKsc-Regular.otf' = '2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b'
    'licenses\fonts\noto-cjk\OFL.txt' = '6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2'
}
foreach ($relative in $fontHashes.Keys) {
    $actualHash = (Get-FileHash -LiteralPath (Join-Path $source $relative) -Algorithm SHA256).Hash
    if ($actualHash -ine $fontHashes[$relative]) {
        throw "Unvalidated bundled font or license: $relative"
    }
}

[void][IO.Directory]::CreateDirectory($stage)
foreach ($file in @('launcher.exe','kyty_emulator.exe','Start-GT7-Desktop.ps1','Start-GT7-Desktop.cmd','LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $source $file) -Destination $stage
}
[void][IO.Directory]::CreateDirectory((Join-Path $stage 'fonts'))
Copy-Item -LiteralPath (Join-Path $source 'fonts\NotoSansCJKsc-Regular.otf') -Destination (Join-Path $stage 'fonts')
Get-ChildItem -LiteralPath $source -Filter '*.dll' -File | ForEach-Object {
    if ($_.Name -notmatch '^(Qt6[A-Za-z0-9_]+|icu[a-z0-9_]*|concrt140|msvcp140(?:_[a-z0-9_]+)?|vcruntime140(?:_[a-z0-9_]+)?|vccorlib140|libwinpthread-1|kyty_dlss_backend|nvngx_dlssg?|NvLowLatencyVk|sl\.(?:interposer|common|dlss_g|reflex|pcl)|amd_fidelityfx_vk)\.dll$') {
        throw "Unexpected DLL in release installation: $($_.Name)"
    }
    Copy-Item -LiteralPath $_.FullName -Destination $stage
}
# launcher.exe and its desktop shortcut script are the release entry points. Capture/playtest/legacy VR scripts
# remain available in the source repository, never in the player package.
foreach ($directory in @('assets','docs','patches','licenses','generic',
    'iconengines','imageformats','networkinformation','platforms','styles','tls',
    'translations')) {
    $path = Join-Path $source $directory
    if (Test-Path -LiteralPath $path -PathType Container) {
        $linked = @(Get-Item -LiteralPath $path) + @(Get-ChildItem -LiteralPath $path -Recurse -Force)
        foreach ($item in $linked) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Release content must not contain directory links: $($item.FullName)"
            }
        }
        Copy-Item -LiteralPath $path -Destination $stage -Recurse
    }
}
foreach ($document in @('README.md','CHANGELOG.md','RELEASE-NOTES.md')) {
    $path = Join-Path $source $document
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        Copy-Item -LiteralPath $path -Destination $stage
    }
}

$files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName)
$sums = foreach ($file in $files) {
    $relative = $file.FullName.Substring($stage.Length + 1).Replace('\','/')
    if ($file.Extension -in @('.ttf','.otf','.ttc','.otc','.woff','.woff2','.eot','.fon','.fnt') -and
        $relative -ine 'fonts/NotoSansCJKsc-Regular.otf') {
        throw "Unexpected font in release package: $relative"
    }
    if ($relative -match '(^|/)(eboot\.bin|gt7save\.bin|_SaveData|_DownloadData|_Trophies|caches?|shader[-_]cache|_PipelineCache|captures?|logs?|outputs)(/|$)' -or
        $file.Extension -in @('.pdb','.map','.log','.dmp','.gpudmp','.bin','.raw','.rgba16f','.ini')) {
        throw "Game, user state or diagnostic content found in release: $relative"
    }
    if ($file.Extension -in @('.md','.txt','.json','.ps1','.cmd','.xml')) {
        $content = [IO.File]::ReadAllText($file.FullName)
        if ($content -match '(?i)[a-z]:[\\/](Users|Documents and Settings)[\\/]') {
            throw "Builder-specific absolute path found in release: $relative"
        }
    }
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $relative"
}
[IO.File]::WriteAllLines((Join-Path $stage 'SHA256SUMS.txt'), [string[]]$sums,
    (New-Object System.Text.UTF8Encoding($false)))
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
$digest = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($archive + '.sha256'), ($digest + '  ' + [IO.Path]::GetFileName($archive) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))
$manifest = [ordered]@{
    product = 'Tohi-GT7'
    version = $version
    tag = $ReleaseTag
    archive = [IO.Path]::GetFileName($archive)
    bytes = (Get-Item -LiteralPath $archive).Length
    sha256 = $digest
    files = $files.Count + 1
    staged_directory = [IO.Path]::GetFileName($stage)
    desktop_preset = '1920x1080 -> 3840x2160 native PSSR inputs to NVIDIA DLSS, preset M, optional fixed NVIDIA 2X frame generation'
    supported_gpu_families = @('GeForce RTX 40','GeForce RTX 50')
    tested_gpu = 'GeForce RTX 5080'
    all_supported_gpu_models_verified = $false
    frame_generation_provider = 'nvidia-streamline-vulkan'
    frame_generation_requested_multiplier = 2
    frame_generation_requested_generated_frames_per_real = 1
    higher_mfg_multipliers_available = $false
    generation_request_is_not_runtime_verification = $true
    backend_sha256 = $backendHash
    nvidia_runtime_verification = $vendorEvidence
    nvidia_license_sha256 = $licenseHashes
    dlss_runtime_file_version = $dlssVersion
    vendor_runtime_sha256 = $vendorHashes
    font_asset_sha256 = $fontHashes
    created_utc = [DateTime]::UtcNow.ToString('o')
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'release-manifest.json') -Encoding UTF8
$manifest | ConvertTo-Json -Depth 6
