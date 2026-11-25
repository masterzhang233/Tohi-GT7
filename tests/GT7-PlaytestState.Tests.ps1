[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$FixtureRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\scripts\GT7-PlaytestState.ps1')
$fixture = [IO.Path]::GetFullPath($FixtureRoot)
if (Test-Path -LiteralPath $fixture) { throw "Use a new fixture directory: $fixture" }
[void][IO.Directory]::CreateDirectory($fixture)
$checks = 0
function Assert-Test([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:checks++
}
function Write-Fixture([string]$Path, [string]$Text) {
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    [IO.File]::WriteAllText($Path, $Text)
}
function Write-DriverCacheFixture([string]$Root, [string]$Name='PPSA01317.bin') {
    # Synthetic envelope only; the engine's Vulkan/hash validation is not run.
    $prefix = [Text.Encoding]::ASCII.GetBytes("KytyPC2:000010de:00002c02:9b438000:3d19fb82df5fbc027e606abe8f8f9c7a`n")
    $bytes = New-Object byte[] ($prefix.Length + 8 + 64)
    [Array]::Copy($prefix, $bytes, $prefix.Length)
    $path = Join-Path $Root (Join-Path '_PipelineCache' $Name)
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $path))
    [IO.File]::WriteAllBytes($path, $bytes)
}
function New-Run([string]$Root, [string]$Name, [string]$SaveTime, [string]$FinishTime,
                 [string]$Status='finished', [int]$ExitCode=0, [string]$Mode='playtest',
                 [string]$Contents='saved-progress', [int]$ProcessId=0) {
    $run = Join-Path $Root $Name
    $save = Join-Path $run '_DownloadData\PPSA01317\gt7save.bin'
    Write-Fixture $save $Contents
    [IO.File]::SetLastWriteTimeUtc($save, [DateTimeOffset]::Parse($SaveTime).UtcDateTime)
    Write-Fixture (Join-Path $run '_SaveData\PPSA01317\slot.bin') $Name
    Write-Fixture (Join-Path $run '_Trophies\PPSA01317\trophies.json') $Name
    $manifest = @{kind='gt7_dlss_experimental_playtest';status=$Status;exit_code=$ExitCode;
                  mode=$Mode;finished_utc=$FinishTime;pid=$ProcessId}
    Write-Fixture (Join-Path $run 'playtest-manifest.json') ($manifest | ConvertTo-Json)
    return $run
}
$seed = Join-Path $fixture 'seed'
Write-Fixture (Join-Path $seed '_SaveData\PPSA01317\slot.bin') 'seed-save'
Write-Fixture (Join-Path $seed '_ShaderCache\PPSA01317\seed.bin') 'seed-shader'
Write-Fixture (Join-Path $seed '_ShaderCache\OTHER-TITLE\other.bin') 'unrelated-shader'
$srRoot = Join-Path $fixture 'sr-runs'
$fgRoot = Join-Path $fixture 'fg-runs'
$old = New-Run $srRoot 'older-save-later-finish' '2026-11-01T01:00:00Z' '2026-11-04T22:00:00Z'
$expected = New-Run $fgRoot 'newest-good' '2026-11-04T20:00:00Z' '2026-11-04T20:01:00Z' -Contents 'career-buycar-cafe-complete'
$failed = New-Run $fgRoot 'failed' '2026-11-04T23:00:00Z' '2026-11-04T23:01:00Z' -ExitCode 1
$running = New-Run $fgRoot 'running' '2026-11-04T23:00:00Z' '2026-11-04T23:01:00Z' -Status 'running'
$active = New-Run $fgRoot 'stale-finished-but-active' '2026-11-04T23:00:00Z' '2026-11-04T23:01:00Z' -ProcessId $PID
$graphics = New-Run $fgRoot 'graphics' '2026-11-04T23:00:00Z' '2026-11-04T23:01:00Z' -Mode 'graphics_check_only'
$empty = New-Run $fgRoot 'empty-save' '2026-11-04T23:00:00Z' '2026-11-04T23:01:00Z' -Contents ''
$malformed = New-Run $fgRoot 'malformed-manifest' '2026-11-04T23:00:00Z' '2026-11-04T23:01:00Z'
[IO.File]::WriteAllText((Join-Path $malformed 'playtest-manifest.json'), '{broken')
$nested = New-Run (Join-Path $fgRoot 'not-a-run') 'nested-run' '2026-11-04T23:00:00Z' '2026-11-04T23:01:00Z'
$before = @(Get-ChildItem -LiteralPath $fixture -Recurse -Force).Count
$selection = Select-GT7PlaytestState -SourceUserDataRoot $seed -OutputRoot $srRoot -ProgressSearchRoots @($srRoot,$fgRoot)
Assert-Test ($selection.selected_root -eq $expected) 'newest actual save wins across SR/FG roots; unsafe runs excluded'
Assert-Test ($selection.selection -eq 'completed_playtest' -and $selection.save_bytes -gt 0 -and $selection.save_sha256.Length -eq 64) 'selection records actual save metadata'
Assert-Test ($selection.source_finished_utc -eq '2026-11-04T20:01:00.0000000Z') 'completion timestamp retains UTC across PowerShell JSON versions'
Assert-Test (@(Get-ChildItem -LiteralPath $fixture -Recurse -Force).Count -eq $before) 'selection creates no directories or files'
$default = Select-GT7PlaytestState -SourceUserDataRoot $seed -OutputRoot $srRoot
Assert-Test ($default.selected_root -eq $old -and $default.search_roots.Count -eq 1) 'default search root is OutputRoot'
$tied = New-Run $srRoot 'same-save-later-completion' '2026-11-04T20:00:00Z' '2026-11-04T20:02:00Z'
$tieSelection = Select-GT7PlaytestState -SourceUserDataRoot $seed -OutputRoot $srRoot -ProgressSearchRoots @($srRoot,$fgRoot)
Assert-Test ($tieSelection.selected_root -eq $tied) 'equal save timestamps use successful completion timestamp'
$fallback = Select-GT7PlaytestState -SourceUserDataRoot $seed -OutputRoot (Join-Path $fixture 'missing-runs')
Assert-Test ($fallback.selected_root -eq $seed -and $fallback.selection -eq 'seed_fallback' -and $null -eq $fallback.save_sha256) 'legacy seed without gt7save is safe fallback'
$seedCandidate = Select-GT7PlaytestState -SourceUserDataRoot $expected -OutputRoot (Join-Path $fixture 'missing-runs')
Assert-Test ($seedCandidate.selected_root -eq $expected -and $seedCandidate.selection -eq 'completed_playtest') 'qualified seed participates as a candidate'
$activeSeedRefused = $false
try { $null = Select-GT7PlaytestState -SourceUserDataRoot $active -OutputRoot (Join-Path $fixture 'missing-runs') } catch { $activeSeedRefused = $true }
Assert-Test $activeSeedRefused 'active seed cannot be used through fallback'
$destination = Join-Path $fixture 'copy-target'
[void][IO.Directory]::CreateDirectory($destination)
$originalHash = (Get-FileHash -LiteralPath $selection.save_path).Hash
Copy-GT7PlaytestState -Selection $selection -DestinationRoot $destination
Assert-Test ((Get-FileHash -LiteralPath (Join-Path $destination '_DownloadData\PPSA01317\gt7save.bin')).Hash -eq $originalHash) 'actual DownloadData career save copied intact'
Assert-Test ([IO.File]::ReadAllText((Join-Path $destination '_SaveData\PPSA01317\slot.bin')) -eq 'newest-good') 'SaveData comes from chosen progress'
Assert-Test ([IO.File]::ReadAllText((Join-Path $destination '_Trophies\PPSA01317\trophies.json')) -eq 'newest-good') 'trophies come from chosen progress'
Assert-Test ([IO.File]::ReadAllText((Join-Path $destination '_ShaderCache\PPSA01317\seed.bin')) -eq 'seed-shader') 'seed cache is fallback when completed runs have no GT7 cache'
Assert-Test (-not (Test-Path -LiteralPath (Join-Path $destination '_ShaderCache\OTHER-TITLE'))) 'unrelated title caches are not copied'
Assert-Test ((Get-FileHash -LiteralPath $selection.save_path).Hash -eq $originalHash) 'original progress is never overwritten'
$overwriteRefused = $false
try { Copy-GT7PlaytestState -Selection $selection -DestinationRoot $destination } catch { $overwriteRefused = $true }
Assert-Test $overwriteRefused 'nonempty destination is never overwritten'
$changedDestination = Join-Path $fixture 'changed-source-target'
[void][IO.Directory]::CreateDirectory($changedDestination)
[IO.File]::AppendAllText($selection.save_path, '-changed-after-selection')
$changedRefused = $false
try { Copy-GT7PlaytestState -Selection $selection -DestinationRoot $changedDestination } catch { $changedRefused = $true }
Assert-Test $changedRefused 'source save changed after selection fails before copying'
Assert-Test (@(Get-ChildItem -LiteralPath $changedDestination -Force).Count -eq 0) 'changed source leaves destination empty'
# Junctions require no Developer Mode on Windows. Keep fixtures for inspection;
# this test deliberately performs no recursive cleanup or deletion.
$linked = New-Run $fgRoot 'linked-state' '2026-11-04T23:00:00Z' '2026-11-04T23:01:00Z'
$junction = Join-Path $linked '_DownloadData\PPSA01317\linked-directory'
$null = New-Item -ItemType Junction -Path $junction -Target $seed
Assert-Test ($null -eq (Get-GT7CompletedProgressCandidate -Root $linked)) 'nested state junction makes a candidate ineligible'
$aliasRoot = Join-Path $fixture 'linked-search'
$null = New-Item -ItemType Junction -Path $aliasRoot -Target $fgRoot
$linkedSelection = Select-GT7PlaytestState -SourceUserDataRoot $seed -OutputRoot $aliasRoot
Assert-Test ($linkedSelection.selection -eq 'seed_fallback') 'linked search roots are not followed'

# Shader warmth follows successful completion, not the newest career save.
$cacheRoot = Join-Path $fixture 'cache-runs'
$career = New-Run $cacheRoot 'newest-career' '2026-11-05T08:00:00Z' '2026-11-05T08:01:00Z' -Contents 'latest-career'
$warm = New-Run $cacheRoot 'warm-race-no-new-save' '2026-11-05T07:00:00Z' '2026-11-05T09:01:00Z'
Write-Fixture (Join-Path $warm '_ShaderCache\PPSA01317\warm.bin') 'experimental-build-shader'
$noEntries = New-Run $cacheRoot 'later-markers-only' '2026-11-05T07:00:00Z' '2026-11-05T10:01:00Z'
Write-Fixture (Join-Path $noEntries '_ShaderCache\PPSA01317\pruned_1234') 'marker'
Write-Fixture (Join-Path $noEntries '_ShaderCache\PPSA01317\empty.bin') ''
Write-Fixture (Join-Path $noEntries '_ShaderCache\OTHER-TITLE\other.bin') 'not-gt7'
$cacheFailed = New-Run $cacheRoot 'later-failed' '2026-11-05T07:00:00Z' '2026-11-05T11:01:00Z' -ExitCode 1
$cacheRunning = New-Run $cacheRoot 'later-running' '2026-11-05T07:00:00Z' '2026-11-05T12:01:00Z' -Status 'running'
$cacheGraphics = New-Run $cacheRoot 'later-graphics-check' '2026-11-05T07:00:00Z' '2026-11-05T13:01:00Z' -Mode 'graphics_check_only'
$cacheActive = New-Run $cacheRoot 'later-active-pid' '2026-11-05T07:00:00Z' '2026-11-05T14:01:00Z' -ProcessId $PID
$cacheLinked = New-Run $cacheRoot 'later-cache-link' '2026-11-05T07:00:00Z' '2026-11-05T15:01:00Z'
foreach ($root in @($cacheFailed,$cacheRunning,$cacheGraphics,$cacheActive,$cacheLinked)) {
    Write-Fixture (Join-Path $root '_ShaderCache\PPSA01317\invalid-source.bin') 'unsafe-cache-source'
}
$null = New-Item -ItemType Junction -Path (Join-Path $cacheLinked '_ShaderCache\PPSA01317\linked') -Target $seed
$cacheSelection = Select-GT7PlaytestState -SourceUserDataRoot $seed -OutputRoot $cacheRoot
Assert-Test ($cacheSelection.selected_root -eq $career) 'cache selection does not change newest career progress'
Assert-Test ($cacheSelection.shader_cache_source -eq $warm -and $cacheSelection.shader_cache_selection -eq 'completed_playtest') 'latest successful nonempty safe GT7 cache beats seed; failed/running/check/linked/empty caches excluded'
Assert-Test ($cacheSelection.shader_cache_finished_utc -eq '2026-11-05T09:01:00.0000000Z') 'cache provenance records completion independently of save timestamp'
$cacheDestination = Join-Path $fixture 'warm-copy-target'
[void][IO.Directory]::CreateDirectory($cacheDestination)
$warmHash = (Get-FileHash -LiteralPath (Join-Path $warm '_ShaderCache\PPSA01317\warm.bin')).Hash
Copy-GT7PlaytestState -Selection $cacheSelection -DestinationRoot $cacheDestination
Assert-Test ([IO.File]::ReadAllText((Join-Path $cacheDestination '_DownloadData\PPSA01317\gt7save.bin')) -eq 'latest-career') 'independent cache copy retains newest career'
Assert-Test ((Get-FileHash -LiteralPath (Join-Path $cacheDestination '_ShaderCache\PPSA01317\warm.bin')).Hash -eq $warmHash) 'completed experimental cache copied intact'
Assert-Test (-not (Test-Path -LiteralPath (Join-Path $cacheDestination '_ShaderCache\PPSA01317\seed.bin'))) 'warm cache does not revert to seed during copy'
Assert-Test ((Get-FileHash -LiteralPath (Join-Path $warm '_ShaderCache\PPSA01317\warm.bin')).Hash -eq $warmHash) 'source experimental cache remains untouched'
$cacheManifestPath = Join-Path $warm 'playtest-manifest.json'
$cacheManifest = Get-Content -LiteralPath $cacheManifestPath -Raw | ConvertFrom-Json
$cacheManifest.status = 'running'
Write-Fixture $cacheManifestPath ($cacheManifest | ConvertTo-Json)
$unsafeCacheDestination = Join-Path $fixture 'cache-became-running-target'
[void][IO.Directory]::CreateDirectory($unsafeCacheDestination)
$unsafeCacheRefused = $false
try { Copy-GT7PlaytestState -Selection $cacheSelection -DestinationRoot $unsafeCacheDestination } catch { $unsafeCacheRefused = $true }
Assert-Test ($unsafeCacheRefused -and @(Get-ChildItem -LiteralPath $unsafeCacheDestination -Force).Count -eq 0) 'cache source becoming active is rejected before any state copy'
$cacheFallback = Select-GT7PlaytestState -SourceUserDataRoot $seed -OutputRoot $cacheRoot
Assert-Test ($cacheFallback.shader_cache_source -eq $seed -and $cacheFallback.shader_cache_selection -eq 'seed_fallback') 'seed remains fallback when every experimental cache is unavailable'
$emptySeed = Join-Path $fixture 'empty-seed'
[void][IO.Directory]::CreateDirectory($emptySeed)
$noCache = Select-GT7PlaytestState -SourceUserDataRoot $emptySeed -OutputRoot (Join-Path $fixture 'missing-cache-runs')
Assert-Test ($null -eq $noCache.shader_cache_source -and $noCache.shader_cache_selection -eq 'none') 'no usable cache is represented explicitly without inventing a source'
Assert-Test ($null -eq $noCache.driver_cache_source -and $noCache.driver_cache_selection -eq 'none' -and $noCache.driver_cache_relative_files.Count -eq 0) 'no driver cache is represented explicitly'

# Driver cache source may differ from both the save and shader cache source.
$driverRoot = Join-Path $fixture 'driver-runs'
$driverCareer = New-Run $driverRoot 'career' '2026-11-06T08:00:00Z' '2026-11-06T08:01:00Z'
$driverWarm = New-Run $driverRoot 'driver-saved' '2026-11-06T07:00:00Z' '2026-11-06T09:01:00Z'
$shaderWarm = New-Run $driverRoot 'later-shaders-only' '2026-11-06T07:00:00Z' '2026-11-06T10:01:00Z'
Write-Fixture (Join-Path $shaderWarm '_ShaderCache\PPSA01317\latest.bin') 'newest-shader'
Write-DriverCacheFixture $driverWarm
Write-DriverCacheFixture $driverWarm 'PPSA01317.1.bin'
Write-Fixture (Join-Path $driverWarm '_PipelineCache\PPSA01317.bin.full') ''
Write-Fixture (Join-Path $driverWarm '_PipelineCache\PPSA01317.1.bin.full') ''
Write-Fixture (Join-Path $driverWarm '_PipelineCache\PPSA01317.bin.tmp') 'unfinished'
Write-DriverCacheFixture $driverWarm 'OTHER-TITLE.bin'
Write-DriverCacheFixture $driverWarm 'PPSA01317.8.bin'
$driverFailed = New-Run $driverRoot 'failed' '2026-11-06T07:00:00Z' '2026-11-06T11:01:00Z' -ExitCode 1
$driverActive = New-Run $driverRoot 'active-pid' '2026-11-06T07:00:00Z' '2026-11-06T12:01:00Z' -ProcessId $PID
$driverRunning = New-Run $driverRoot 'running' '2026-11-06T07:00:00Z' '2026-11-06T13:01:00Z' -Status 'running'
$driverGraphics = New-Run $driverRoot 'graphics-check' '2026-11-06T07:00:00Z' '2026-11-06T14:01:00Z' -Mode 'graphics_check_only'
$driverLinked = New-Run $driverRoot 'linked' '2026-11-06T07:00:00Z' '2026-11-06T15:01:00Z'
foreach ($root in @($driverFailed,$driverActive,$driverRunning,$driverGraphics,$driverLinked)) { Write-DriverCacheFixture $root }
$null = New-Item -ItemType Junction -Path (Join-Path $driverLinked '_PipelineCache\linked') -Target $seed
$driverBad = New-Run $driverRoot 'bad-envelope' '2026-11-06T07:00:00Z' '2026-11-06T16:01:00Z'
Write-Fixture (Join-Path $driverBad '_PipelineCache\PPSA01317.bin') 'invalid'
$driverEmpty = New-Run $driverRoot 'empty' '2026-11-06T07:00:00Z' '2026-11-06T17:01:00Z'
Write-Fixture (Join-Path $driverEmpty '_PipelineCache\PPSA01317.bin') ''
Write-Fixture (Join-Path $driverEmpty '_PipelineCache\PPSA01317.bin.full') ''
$driverSplit = New-Run $driverRoot 'split-without-base' '2026-11-06T07:00:00Z' '2026-11-06T18:01:00Z'
Write-DriverCacheFixture $driverSplit 'PPSA01317.1.bin'
Write-DriverCacheFixture $seed
$driverSelection = Select-GT7PlaytestState -SourceUserDataRoot $seed -OutputRoot $driverRoot
Assert-Test ($driverSelection.selected_root -eq $driverCareer -and $driverSelection.shader_cache_source -eq $shaderWarm -and $driverSelection.driver_cache_source -eq $driverWarm) 'save, shader and driver sources are independently selected; unsafe, invalid, empty and graphics-check driver sources excluded'
Assert-Test ($driverSelection.driver_cache_selection -eq 'completed_playtest' -and $driverSelection.driver_cache_finished_utc -eq '2026-11-06T09:01:00.0000000Z') 'driver provenance records source completion'
Assert-Test ($driverSelection.driver_cache_relative_files.Count -eq 4) 'driver selection includes real split/full names, excludes temporary files, unsupported indices and other titles'
$driverDestination = Join-Path $fixture 'driver-copy-target'
[void][IO.Directory]::CreateDirectory($driverDestination)
$driverHash = (Get-FileHash -LiteralPath (Join-Path $driverWarm '_PipelineCache\PPSA01317.bin')).Hash
Copy-GT7PlaytestState -Selection $driverSelection -DestinationRoot $driverDestination
Assert-Test ((Get-FileHash -LiteralPath (Join-Path $driverDestination '_PipelineCache\PPSA01317.bin')).Hash -eq $driverHash) 'driver base copied intact'
Assert-Test ((Test-Path -LiteralPath (Join-Path $driverDestination '_PipelineCache\PPSA01317.1.bin')) -and (Test-Path -LiteralPath (Join-Path $driverDestination '_PipelineCache\PPSA01317.bin.full')) -and (Test-Path -LiteralPath (Join-Path $driverDestination '_PipelineCache\PPSA01317.1.bin.full'))) 'driver split and full markers are preserved'
Assert-Test (@(Get-ChildItem -LiteralPath (Join-Path $driverDestination '_PipelineCache') -File).Count -eq 4) 'only selected driver cache files copied'
Assert-Test ([IO.File]::ReadAllText((Join-Path $driverDestination '_ShaderCache\PPSA01317\latest.bin')) -eq 'newest-shader') 'driver source does not replace independently selected shader cache'
Assert-Test ((Get-FileHash -LiteralPath (Join-Path $driverWarm '_PipelineCache\PPSA01317.bin')).Hash -eq $driverHash) 'source driver cache remains unchanged'
$driverManifestPath = Join-Path $driverWarm 'playtest-manifest.json'
$driverManifest = Get-Content -LiteralPath $driverManifestPath -Raw | ConvertFrom-Json
$driverManifest.status = 'running'
Write-Fixture $driverManifestPath ($driverManifest | ConvertTo-Json)
$unsafeDriverDestination = Join-Path $fixture 'driver-became-running-target'
[void][IO.Directory]::CreateDirectory($unsafeDriverDestination)
$unsafeDriverRefused = $false
try { Copy-GT7PlaytestState -Selection $driverSelection -DestinationRoot $unsafeDriverDestination } catch { $unsafeDriverRefused = $true }
Assert-Test ($unsafeDriverRefused -and @(Get-ChildItem -LiteralPath $unsafeDriverDestination -Force).Count -eq 0) 'driver source becoming active is refused before copying state'
$driverFallback = Select-GT7PlaytestState -SourceUserDataRoot $seed -OutputRoot $driverRoot
Assert-Test ($driverFallback.driver_cache_source -eq $seed -and $driverFallback.driver_cache_selection -eq 'seed_fallback') 'driver seed fallback retains engine compatibility checks'
Write-Output "GT7 playtest progress checks passed: $checks; fixtures retained at $fixture"
