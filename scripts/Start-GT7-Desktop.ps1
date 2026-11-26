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
    $required = @('launcher.exe', 'kyty_emulator.exe', 'kyty_dlss_backend.dll', 'nvngx_dlss.dll')
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
            graphics_preset = 'Use the saved graphics settings: native PSSR inputs, DLSS 2560x1440 to 3840x2160 reconstruction, preset M, fullscreen Mailbox at the desktop display size. External capture DLSS frame generation 2X Auto is enabled by default and can be disabled in the launcher. Internal FG/MFG and extra FPS caps are disabled. Supported GPUs: GeForce RTX 40/50 series.'
            external_frame_generation_component_present = Test-Path -LiteralPath (Join-Path $install 'framegen\TohiFrameGen.exe') -PathType Leaf
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
