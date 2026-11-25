[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$PowerShellExecutable = (Get-Process -Id $PID).Path,
    [string]$LauncherScript = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scripts = Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts'
if ([string]::IsNullOrWhiteSpace($LauncherScript)) { $LauncherScript = Join-Path $scripts 'GT7-Test-DLSS.ps1' }
. (Join-Path $scripts 'GT7-LaunchSupport.ps1')
$root = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('log-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
[void][IO.Directory]::CreateDirectory($root)
$result = [ordered]@{ passed=$false; game_started=$false; gpu_initialized=$false; powershell=$PowerShellExecutable;
    launcher_sha256=(Get-FileHash -LiteralPath $LauncherScript).Hash; checks=@() }
$child = $null
$ready = Join-Path $root 'ready'
$release = Join-Path $root 'release'
try {
    # Compile a .NET Framework console-only producer, usable from PS5.1 and PS7.
    # The real launcher invokes it for both preflight and the fixture run. It has
    # no emulator, graphics or game dependencies, and never reads fake eboot.bin.
    $code = @'
using System;
using System.IO;
using System.Text;
using System.Threading;
public class Gt7LogProducer {
    static void Text(Stream stream, string text) {
        byte[] bytes = Encoding.ASCII.GetBytes(text); stream.Write(bytes, 0, bytes.Length); stream.Flush();
    }
    static void Burst(Stream stream, byte value, string tail) {
        byte[] chunk = new byte[4096]; for (int i=0; i<chunk.Length; ++i) chunk[i]=value;
        for (int i=0; i<128; ++i) stream.Write(chunk, 0, chunk.Length);
        Text(stream, tail);
    }
    public static int Main(string[] args) {
        if (Array.IndexOf(args, "--gt7-check-only") >= 0) return 0;
        string ready=Environment.GetEnvironmentVariable("LOG_FIXTURE_READY");
        string release=Environment.GetEnvironmentVariable("LOG_FIXTURE_RELEASE");
        if (String.IsNullOrEmpty(ready) || String.IsNullOrEmpty(release)) return 97;
        Stream output=Console.OpenStandardOutput(), error=Console.OpenStandardError();
        Text(output, new String('A', 8192));
        Thread.Sleep(200);
        Text(output, "LIVE_STDOUT\n"); Text(error, "LIVE_STDERR\n");
        File.WriteAllText(ready, "both small chunks flushed by producer");
        DateTime deadline=DateTime.UtcNow.AddSeconds(20);
        while (!File.Exists(release)) { if (DateTime.UtcNow>=deadline) return 98; Thread.Sleep(10); }
        Thread stdoutThread=new Thread(delegate() { Burst(output, (byte)'O', "TAIL_STDOUT\n"); });
        Thread stderrThread=new Thread(delegate() { Burst(error, (byte)'E', "TAIL_STDERR\n"); });
        stdoutThread.Start(); stderrThread.Start(); stdoutThread.Join(); stderrThread.Join();
        return 0;
    }
}
'@
    $source = Join-Path $root 'producer.cs'
    $probe = Join-Path $root 'producer.exe'
    [IO.File]::WriteAllText($source, $code)
    & (Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe') /nologo /target:exe (('/out:' + $probe)) $source
    if ($LASTEXITCODE -ne 0) { throw 'Log producer compilation failed.' }
    $scriptCopy = Join-Path $root 'scripts'
    $fixture = Join-Path $root 'fixture'
    $seed = Join-Path $root 'seed'
    $runs = Join-Path $root 'runs'
    foreach ($path in @($scriptCopy,$fixture,(Join-Path $seed '_DownloadData\PPSA01317'))) {
        [void][IO.Directory]::CreateDirectory($path)
    }
    Copy-Item -LiteralPath $LauncherScript -Destination (Join-Path $scriptCopy 'GT7-Test-DLSS.ps1')
    foreach ($name in @('GT7-LaunchSupport.ps1','GT7-PlaytestState.ps1')) {
        Copy-Item -LiteralPath (Join-Path $scripts $name) -Destination (Join-Path $scriptCopy $name)
    }
    foreach ($name in @('eboot.bin','backend.dll','nvngx_dlss.dll')) {
        [IO.File]::WriteAllText((Join-Path $fixture $name), 'Offline fixture; never load or execute.')
    }
    [IO.File]::WriteAllText((Join-Path $seed '_DownloadData\PPSA01317\gt7save.bin'), 'Offline save fixture.')
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $scriptCopy 'GT7-Test-DLSS.ps1'),
        '-GamePath',$fixture,'-EmulatorPath',$probe,'-BackendPath',(Join-Path $fixture 'backend.dll'),
        '-RuntimeDirectory',$fixture,'-SourceUserDataRoot',$seed,'-OutputRoot',$runs,'-NativePssr','-NativePssr1080To4K')
    $child = New-Object System.Diagnostics.Process
    $child.StartInfo = New-GT7ProcessStartInfo -Executable $PowerShellExecutable -ArgumentList $arguments -WorkingDirectory $root -CaptureOutput
    $child.StartInfo.EnvironmentVariables['LOG_FIXTURE_READY'] = $ready
    $child.StartInfo.EnvironmentVariables['LOG_FIXTURE_RELEASE'] = $release
    [void]$child.Start()
    $launcherOutput = $child.StandardOutput.ReadToEndAsync()
    $launcherError = $child.StandardError.ReadToEndAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path -LiteralPath $ready) -and -not $child.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 20
    }
    if (-not (Test-Path -LiteralPath $ready)) { throw 'Fixture never reached its live-output barrier.' }
    $manifests = @(Get-ChildItem -LiteralPath $runs -Recurse -File -Filter 'playtest-manifest.json')
    if ($manifests.Count -ne 1) { throw 'Expected exactly one fixture run.' }
    $manifestPath = $manifests[0].FullName
    $run = Split-Path -Parent $manifestPath
    function Read-LiveText([string]$Path) {
        $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
        $reader = New-Object IO.StreamReader($stream)
        try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(2)
    do {
        $liveOut = Read-LiveText (Join-Path $run 'stdout.log')
        $liveErr = Read-LiveText (Join-Path $run 'stderr.log')
        $visible = $liveOut.EndsWith("LIVE_STDOUT`n") -and $liveErr.EndsWith("LIVE_STDERR`n")
        if (-not $visible) { Start-Sleep -Milliseconds 20 }
    } while (-not $visible -and [DateTime]::UtcNow -lt $deadline)
    $liveManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $livePassed = $visible -and -not $child.HasExited -and $liveManifest.status -ceq 'running'
    $result.checks += [ordered]@{ name='small-chunks-visible-while-running'; passed=$livePassed;
        stdout_bytes=$liveOut.Length; stderr_bytes=$liveErr.Length; manifest_status=$liveManifest.status }
    # Release even the negative control, so it must also prove correct final tails
    # and completion rather than leaving a child behind on the expected failure.
    [IO.File]::WriteAllText($release, 'continue')
    $complete = $child.WaitForExit(15000)
    $result.checks += [ordered]@{name='concurrent-stdout-stderr-over-pipe-capacity';passed=$complete;bytes_per_burst=524288}
    if (-not $complete) { throw 'Dual-pipe fixture did not complete within 15 seconds.' }
    [IO.File]::WriteAllText((Join-Path $root 'launcher.stdout.txt'), $launcherOutput.GetAwaiter().GetResult())
    [IO.File]::WriteAllText((Join-Path $root 'launcher.stderr.txt'), $launcherError.GetAwaiter().GetResult())
    $finalOut = [IO.File]::ReadAllText((Join-Path $run 'stdout.log'))
    $finalErr = [IO.File]::ReadAllText((Join-Path $run 'stderr.log'))
    $expectedOut = ('A' * 8192) + "LIVE_STDOUT`n" + ('O' * 524288) + "TAIL_STDOUT`n"
    $expectedErr = "LIVE_STDERR`n" + ('E' * 524288) + "TAIL_STDERR`n"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $tailsPassed = $finalOut -ceq $expectedOut -and $finalErr -ceq $expectedErr -and
        $child.ExitCode -eq 0 -and $manifest.status -ceq 'finished' -and $manifest.exit_code -eq 0 -and
        -not [string]::IsNullOrWhiteSpace($manifest.finished_utc)
    $result.checks += [ordered]@{name='exact-final-bytes-and-finished-manifest';passed=$tailsPassed;
        stdout_bytes=$finalOut.Length;stderr_bytes=$finalErr.Length;manifest_status=$manifest.status;exit_code=$manifest.exit_code}
    if (-not $livePassed -or -not $tailsPassed) { throw 'Output visibility or final log/manifest regression.' }
    $result.passed = $true
} catch {
    $result.error = $_.Exception.Message
    throw
} finally {
    if ($null -ne $child) {
        # These are exclusively fixture processes, never the user's emulator.
        [IO.File]::WriteAllText($release, 'finish fixture cleanup')
        if (-not $child.HasExited) {
            if (-not $child.WaitForExit(5000)) { $child.Kill(); $child.WaitForExit() }
        }
        $child.Dispose()
    }
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'result.json') -Encoding UTF8
    Write-Output ('GT7 log capture test result: ' + (Join-Path $root 'result.json'))
}
