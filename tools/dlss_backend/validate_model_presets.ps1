param(
    [Parameter(Mandatory = $true)][string]$BackendBuild,
    [Parameter(Mandatory = $true)][string]$RuntimeDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
foreach ($path in @($BackendBuild, $RuntimeDirectory, $OutputDirectory)) {
    if (-not [IO.Path]::IsPathRooted($path)) { throw 'All paths must be absolute.' }
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a new output directory to preserve earlier evidence.' }
$smoke = Join-Path $BackendBuild 'kyty_dlss_backend_smoke.exe'
$backend = Join-Path $BackendBuild 'kyty_dlss_backend.dll'
foreach ($path in @($smoke, $backend, (Join-Path $RuntimeDirectory 'nvngx_dlss.dll'))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing input: $path" }
}
$null = New-Item -ItemType Directory -Path $OutputDirectory
$oldPreset = [Environment]::GetEnvironmentVariable('KYTY_GT7_DLSS_PRESET', 'Process')
$oldLogging = [Environment]::GetEnvironmentVariable('KYTY_GT7_DLSS_NGX_LOG', 'Process')
$summary = [ordered]@{
    schema = 1
    passed = $false
    effective_preset_verified = $false
    note = 'Distinct GPU outputs establish model selection affects computation, not an effective-preset query or image-quality claim.'
    runs = @()
}
try {
    # Separate processes prevent NGX lifetime, cached features and environment
    # variables from leaking between models. Verbose logs retain runtime hints.
    [Environment]::SetEnvironmentVariable('KYTY_GT7_DLSS_NGX_LOG', '1', 'Process')
    foreach ($preset in @('default', 'K', 'M', 'L')) {
        $value = if ($preset -eq 'default') { $null } else { $preset }
        [Environment]::SetEnvironmentVariable('KYTY_GT7_DLSS_PRESET', $value, 'Process')
        $runDirectory = Join-Path $OutputDirectory $preset
        $consoleLog = Join-Path $OutputDirectory "$preset-console.log"
        & $smoke $backend $RuntimeDirectory $runDirectory --quality-1440p *> $consoleLog
        if ($LASTEXITCODE -ne 0) { throw "Preset $preset failed; see $consoleLog" }
        $report = Get-Content -Raw -LiteralPath (Join-Path $runDirectory 'smoke-report.json') | ConvertFrom-Json
        if (-not $report.passed -or -not $report.invalid_preset_rejected -or $report.frames.Count -ne 4) {
            throw "Preset $preset did not complete the four-frame contract."
        }
        $hashes = @(0..3 | ForEach-Object {
            (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $runDirectory "smoke-$_.rgba16f")).Hash
        })
        $summary.runs += [ordered]@{
            selection = $preset
            requested_preset = $report.requested_preset
            gpu = $report.gpu
            input = $report.input
            output = $report.output
            frame_sha256 = $hashes
        }
    }
    $default = $summary.runs[0].frame_sha256 -join ','
    if ($default -ne ($summary.runs[1].frame_sha256 -join ',')) {
        throw 'Explicit K differs from the default K path; investigate model selection or runtime overrides.'
    }
    if ($default -eq ($summary.runs[2].frame_sha256 -join ',')) {
        throw 'M is bit-identical to K: do not claim DLSS 4.5 is enabled. Inspect runtime hints and driver overrides.'
    }
    $summary.default_and_explicit_k_match = $true
    $summary.m_differs_from_k = $true
    $summary.passed = $true
} catch {
    $summary.error = $_.Exception.Message
    throw
} finally {
    [Environment]::SetEnvironmentVariable('KYTY_GT7_DLSS_PRESET', $oldPreset, 'Process')
    [Environment]::SetEnvironmentVariable('KYTY_GT7_DLSS_NGX_LOG', $oldLogging, 'Process')
    $summary | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $OutputDirectory 'model-regression.json')
}
$summary | ConvertTo-Json -Depth 8
