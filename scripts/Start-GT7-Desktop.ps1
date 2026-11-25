[CmdletBinding()]
param(
    [string]$InstallDirectory = '',
    [switch]$CheckOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

try {
    if (-not $InstallDirectory) {
        # Packaged beside launcher.exe; source/install layouts may keep this in scripts/.
        $InstallDirectory = $PSScriptRoot
        if (-not (Test-Path -LiteralPath (Join-Path $InstallDirectory 'launcher.exe') -PathType Leaf)) {
            $InstallDirectory = Split-Path -Parent $PSScriptRoot
        }
    }
    $install = (Resolve-Path -LiteralPath $InstallDirectory).ProviderPath
    if (-not (Test-Path -LiteralPath $install -PathType Container)) {
        throw 'InstallDirectory must be the extracted Tohi-GT7 release folder.'
    }
    $required = @('launcher.exe', 'kyty_emulator.exe', 'kyty_dlss_backend.dll',
        'nvngx_dlss.dll', 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll',
        'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll', 'NvLowLatencyVk.dll')
    $missing = @($required | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $install $_) -PathType Leaf)
    })
    if ($missing.Count) {
        throw ('The Tohi-GT7 release is incomplete. Extract the full release package. Missing: ' + ($missing -join ', '))
    }
    $launcher = Join-Path $install 'launcher.exe'
    if ($CheckOnly) {
        [ordered]@{
            product = 'Tohi-GT7'
            entry_point = 'desktop_launcher'
            executable = $launcher
            working_directory = $install
            required_files_present = $true
            graphics_preset = 'Use the saved graphics checkbox: native PSSR inputs, DLSS 1920x1080 to 3840x2160, NVIDIA fixed 2X (one generated frame per rendered frame). Supported GPUs: GeForce RTX 40/50 series.'
            started = $false
            save_files_modified = $false
        } | ConvertTo-Json
        exit 0
    }
    $process = New-Object System.Diagnostics.Process
    try {
        $process.StartInfo.FileName = $launcher
        $process.StartInfo.WorkingDirectory = $install
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.CreateNoWindow = $true
        # An old playtest shell cannot change the release's fixed graphics preset.
        foreach ($key in @($process.StartInfo.EnvironmentVariables.Keys)) {
            if ($key -match '^(KYTY_|GT7_)') {
                $process.StartInfo.EnvironmentVariables.Remove($key)
            }
        }
        if (-not $process.Start()) { throw 'Could not start the Tohi-GT7 launcher.' }
    } finally {
        if ($null -ne $process) { $process.Dispose() }
    }
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
