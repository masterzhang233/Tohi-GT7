# Read-only selection and copy-forward of completed GT7 playtest progress.
# Never writes to a seed or a previous run; every launch receives a new copy.
Set-StrictMode -Version Latest

function Assert-GT7StatePathNoReparse {
    param([Parameter(Mandatory=$true)][string]$LiteralPath)
    $item = Get-Item -LiteralPath $LiteralPath -Force -ErrorAction Stop
    while ($null -ne $item) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Playtest state path contains a junction or symbolic link: $($item.FullName)"
        }
        $parent = Split-Path -Parent $item.FullName
        if (-not $parent -or $parent -eq $item.FullName) { break }
        $item = Get-Item -LiteralPath $parent -Force -ErrorAction Stop
    }
}

function Assert-GT7StateTreeNoReparse {
    param([Parameter(Mandatory=$true)][string]$LiteralPath)
    Assert-GT7StatePathNoReparse -LiteralPath $LiteralPath
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($LiteralPath)
    while ($pending.Count) {
        foreach ($item in @(Get-ChildItem -LiteralPath $pending.Pop() -Force -ErrorAction Stop)) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Playtest state contains a junction or symbolic link: $($item.FullName)"
            }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
        }
    }
}

function Get-GT7CompletedProgressCandidate {
    param([Parameter(Mandatory=$true)][string]$Root)
    try {
        Assert-GT7StatePathNoReparse -LiteralPath $Root
        $manifestPath = Join-Path $Root 'playtest-manifest.json'
        Assert-GT7StatePathNoReparse -LiteralPath $manifestPath
        $manifest = Get-Content -LiteralPath $manifestPath -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
        if ($manifest.kind -ne 'gt7_dlss_experimental_playtest' -or
            $manifest.mode -ne 'playtest' -or $manifest.status -ne 'finished' -or
            $null -eq $manifest.exit_code -or $manifest.exit_code -ne 0) { return $null }
        if ($manifest.PSObject.Properties['graphics_check_only'] -and $manifest.graphics_check_only) { return $null }
        if ($manifest.PSObject.Properties['pid'] -and $manifest.pid) {
            # A reused PID is conservatively excluded too. Never copy a running
            # process's data merely because a stale manifest says "finished".
            $active = Get-Process -Id ([int]$manifest.pid) -ErrorAction SilentlyContinue
            if ($null -ne $active) { return $null }
        }
        # PowerShell 7 may deserialize ISO timestamps into DateTime already;
        # converting that to a string would discard its UTC kind and fractions.
        if ($manifest.finished_utc -is [DateTime]) {
            $finished = $manifest.finished_utc.ToUniversalTime()
        } elseif ($manifest.finished_utc -is [DateTimeOffset]) {
            $finished = $manifest.finished_utc.UtcDateTime
        } else {
            $finished = [DateTimeOffset]::Parse([string]$manifest.finished_utc, [Globalization.CultureInfo]::InvariantCulture).UtcDateTime
        }
        $save = Join-Path $Root '_DownloadData\PPSA01317\gt7save.bin'
        Assert-GT7StatePathNoReparse -LiteralPath $save
        $file = Get-Item -LiteralPath $save -Force -ErrorAction Stop
        if ($file.PSIsContainer -or $file.Length -le 0) { return $null }
        foreach ($relative in @('_SaveData\PPSA01317', '_Trophies\PPSA01317', '_DownloadData\PPSA01317')) {
            $directory = Join-Path $Root $relative
            if (Test-Path -LiteralPath $directory) {
                if (-not (Test-Path -LiteralPath $directory -PathType Container)) { return $null }
                Assert-GT7StateTreeNoReparse -LiteralPath $directory
            }
        }
        return [pscustomobject]@{
            root = [IO.Path]::GetFullPath($Root); save_path = $file.FullName
            save_last_write_utc = $file.LastWriteTimeUtc; finished_utc = $finished
            save_bytes = $file.Length
        }
    } catch {
        # Missing, malformed, linked or inaccessible runs are not safe candidates.
        return $null
    }
}

function Test-GT7ShaderCacheCandidate {
    param([Parameter(Mandatory=$true)][string]$Root)
    try {
        $directory = Join-Path $Root '_ShaderCache\PPSA01317'
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) { return $false }
        Assert-GT7StateTreeNoReparse -LiteralPath $directory
        # A folder containing only pruning markers/call targets is not a warm
        # cache. The engine reads its keyed .bin entries directly in this folder.
        return $null -ne (Get-ChildItem -LiteralPath $directory -File -Filter '*.bin' -Force -ErrorAction Stop |
            Where-Object { $_.Length -gt 0 } | Select-Object -First 1)
    } catch { return $false }
}

function Get-GT7DriverCacheFiles {
    param([Parameter(Mandatory=$true)][string]$Root)
    $directory = Join-Path $Root '_PipelineCache'
    Assert-GT7StateTreeNoReparse -LiteralPath $directory
    # DriverCacheFilePath uses the base file and indices 1..7 (at most eight
    # files). Preserve its .full sidecars, never pending .tmp or other titles.
    Get-ChildItem -LiteralPath $directory -File -Force -ErrorAction Stop |
        Where-Object { $_.Name -cmatch '^PPSA01317(?:\.[1-7])?\.bin(?:\.full)?$' }
}

function Test-GT7DriverCacheCandidate {
    param([Parameter(Mandatory=$true)][string]$Root)
    $stream = $null
    try {
        $base = @(Get-GT7DriverCacheFiles -Root $Root) |
            Where-Object { $_.Name -ceq 'PPSA01317.bin' } | Select-Object -First 1
        # The engine starts at the base file; a lone split/marker is unusable.
        if ($null -eq $base -or $base.Length -gt 3840MB) { return $false }
        $stream = [IO.File]::Open($base.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $prefix = New-Object byte[] 128
        $read = $stream.Read($prefix, 0, $prefix.Length)
        $signature = [regex]::Match([Text.Encoding]::ASCII.GetString($prefix, 0, $read),
            '\AKytyPC2:[0-9a-f]{8}:[0-9a-f]{8}:[0-9a-f]{8}:[0-9a-f]{32}\n')
        # Only an envelope/nonempty check. GPU/driver/UUID and payload hash
        # validation remain with ReadDriverCacheFile; no Vulkan work here.
        return $signature.Success -and $base.Length -gt ($signature.Length + 8)
    } catch { return $false }
    finally { if ($null -ne $stream) { $stream.Dispose() } }
}

function Select-GT7PlaytestState {
    param(
        [Parameter(Mandatory=$true)][string]$SourceUserDataRoot,
        [Parameter(Mandatory=$true)][string]$OutputRoot,
        [string[]]$ProgressSearchRoots
    )
    $seed = (Resolve-Path -LiteralPath $SourceUserDataRoot -ErrorAction Stop).ProviderPath
    Assert-GT7StatePathNoReparse -LiteralPath $seed
    $search = @($ProgressSearchRoots | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if (-not $search.Count) { $search = @($OutputRoot) }
    $search = @($search | ForEach-Object { [IO.Path]::GetFullPath($_) } | Select-Object -Unique)
    $roots = New-Object 'System.Collections.Generic.List[string]'
    $roots.Add($seed)
    foreach ($directory in $search) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) { continue }
        try { Assert-GT7StatePathNoReparse -LiteralPath $directory } catch { continue }
        foreach ($child in @(Get-ChildItem -LiteralPath $directory -Directory -Force -ErrorAction Stop)) {
            if (-not ($child.Attributes -band [IO.FileAttributes]::ReparsePoint)) { $roots.Add($child.FullName) }
        }
    }
    $candidates = @(foreach ($root in @($roots | Select-Object -Unique)) {
        $candidate = Get-GT7CompletedProgressCandidate -Root $root
        if ($null -ne $candidate) { $candidate }
    })
    $selected = $candidates | Sort-Object -Property `
        @{Expression={$_.save_last_write_utc};Descending=$true}, `
        @{Expression={$_.finished_utc};Descending=$true}, `
        @{Expression={$_.root};Descending=$false} | Select-Object -First 1
    $kind = 'completed_playtest'
    if ($null -eq $selected) {
        $kind = 'seed_fallback'
        $seedManifestPath = Join-Path $seed 'playtest-manifest.json'
        if (Test-Path -LiteralPath $seedManifestPath) {
            Assert-GT7StatePathNoReparse -LiteralPath $seedManifestPath
            $seedManifest = Get-Content -LiteralPath $seedManifestPath -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
            if (($seedManifest.PSObject.Properties['status'] -and $seedManifest.status -eq 'running') -or
                ($seedManifest.PSObject.Properties['pid'] -and $seedManifest.pid -and
                 $null -ne (Get-Process -Id ([int]$seedManifest.pid) -ErrorAction SilentlyContinue))) {
                throw 'The fallback seed belongs to an active playtest; close it normally before copying progress.'
            }
        }
        $save = Join-Path $seed '_DownloadData\PPSA01317\gt7save.bin'
        $file = $null
        if (Test-Path -LiteralPath $save) {
            Assert-GT7StatePathNoReparse -LiteralPath $save
            $file = Get-Item -LiteralPath $save -Force -ErrorAction Stop
            if ($file.PSIsContainer) { throw "GT7 save is a directory: $save" }
        }
        $selected = [pscustomobject]@{
            root = $seed; save_path = $(if ($null -ne $file) { $file.FullName } else { $null })
            save_last_write_utc = $(if ($null -ne $file) { $file.LastWriteTimeUtc } else { $null })
            finished_utc = $null; save_bytes = $(if ($null -ne $file) { $file.Length } else { 0 })
        }
    }
    # CheckOnly also validates the selected trees without creating anything.
    foreach ($relative in @('_SaveData\PPSA01317', '_Trophies\PPSA01317', '_DownloadData\PPSA01317')) {
        $directory = Join-Path $selected.root $relative
        if (Test-Path -LiteralPath $directory) {
            if (-not (Test-Path -LiteralPath $directory -PathType Container)) { throw "State directory is not a directory: $directory" }
            Assert-GT7StateTreeNoReparse -LiteralPath $directory
        }
    }
    # Progress freshness and cache warmth are independent: a later race can
    # compile useful shaders without updating the career save. Ignore empty,
    # failed, running and graphics-check runs; never merge into an old run.
    $cache = $null
    foreach ($candidate in @($candidates | Sort-Object -Property `
        @{Expression={$_.finished_utc};Descending=$true}, `
        @{Expression={$_.root};Descending=$false})) {
        if (Test-GT7ShaderCacheCandidate -Root $candidate.root) { $cache = $candidate; break }
    }
    $cacheKind = 'completed_playtest'
    if ($null -eq $cache) {
        $cacheKind = 'none'
        # A legacy seed has no manifest. If it is itself a playtest, accept its
        # cache only through the successfully completed candidate list above.
        if (-not (Test-Path -LiteralPath (Join-Path $seed 'playtest-manifest.json')) -and
            (Test-GT7ShaderCacheCandidate -Root $seed)) {
            $cacheKind = 'seed_fallback'
            $cache = [pscustomobject]@{root=$seed;finished_utc=$null}
        }
    }
    # Driver pipelines can be saved less frequently than shader translations.
    # Select independently so a newer shader-only run does not discard an older
    # successfully saved driver cache.
    $driverCache = $null
    foreach ($candidate in @($candidates | Sort-Object -Property `
        @{Expression={$_.finished_utc};Descending=$true}, `
        @{Expression={$_.root};Descending=$false})) {
        if (Test-GT7DriverCacheCandidate -Root $candidate.root) { $driverCache = $candidate; break }
    }
    $driverKind = 'completed_playtest'
    if ($null -eq $driverCache) {
        $driverKind = 'none'
        if (-not (Test-Path -LiteralPath (Join-Path $seed 'playtest-manifest.json')) -and
            (Test-GT7DriverCacheCandidate -Root $seed)) {
            $driverKind = 'seed_fallback'
            $driverCache = [pscustomobject]@{root=$seed;finished_utc=$null}
        }
    }
    return [pscustomobject]@{
        selection = $kind; selected_root = $selected.root; seed_root = $seed
        search_roots = $search; save_path = $selected.save_path
        save_last_write_utc = $(if ($null -ne $selected.save_last_write_utc) { $selected.save_last_write_utc.ToString('o') } else { $null })
        source_finished_utc = $(if ($null -ne $selected.finished_utc) { $selected.finished_utc.ToString('o') } else { $null })
        save_bytes = $selected.save_bytes
        save_sha256 = $(if ($null -ne $selected.save_path) { (Get-FileHash -LiteralPath $selected.save_path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant() } else { $null })
        shader_cache_selection = $cacheKind
        shader_cache_source = $(if ($null -ne $cache) { $cache.root } else { $null })
        shader_cache_finished_utc = $(if ($null -ne $cache -and $null -ne $cache.finished_utc) { $cache.finished_utc.ToString('o') } else { $null })
        shader_cache_relative_path = '_ShaderCache\PPSA01317'
        shader_cache_compatibility = 'Engine validates format, recompiler source version, GPU/device switches, recompiler environment, exact entry key and payload hash; incompatible entries are ignored. Copying is not proof of cache hits or a stutter-free run.'
        driver_cache_selection = $driverKind
        driver_cache_source = $(if ($null -ne $driverCache) { $driverCache.root } else { $null })
        driver_cache_finished_utc = $(if ($null -ne $driverCache -and $null -ne $driverCache.finished_utc) { $driverCache.finished_utc.ToString('o') } else { $null })
        driver_cache_relative_files = @($(if ($null -ne $driverCache) {
            Get-GT7DriverCacheFiles -Root $driverCache.root | Sort-Object Name | ForEach-Object { Join-Path '_PipelineCache' $_.Name }
        }))
        driver_cache_compatibility = 'Only the file envelope is checked here. Engine validates vendor, device, driver version, pipeline-cache UUID and payload hash before use; Vulkan can reject incompatible data. Copying does not prove cache hits.'
    }
}

function Copy-GT7PlaytestState {
    param(
        [Parameter(Mandatory=$true)]$Selection,
        [Parameter(Mandatory=$true)][string]$DestinationRoot
    )
    Assert-GT7StatePathNoReparse -LiteralPath $DestinationRoot
    if (@(Get-ChildItem -LiteralPath $DestinationRoot -Force).Count) { throw "Playtest destination is not empty: $DestinationRoot" }
    if ($Selection.selection -eq 'completed_playtest' -and
        $null -eq (Get-GT7CompletedProgressCandidate -Root $Selection.selected_root)) {
        throw 'Selected progress is no longer an inactive successfully completed playtest.'
    }
    if ($null -ne $Selection.shader_cache_source) {
        if (($Selection.shader_cache_selection -eq 'completed_playtest' -and
             $null -eq (Get-GT7CompletedProgressCandidate -Root $Selection.shader_cache_source)) -or
            ($Selection.shader_cache_selection -eq 'seed_fallback' -and
             (Test-Path -LiteralPath (Join-Path $Selection.shader_cache_source 'playtest-manifest.json'))) -or
            -not (Test-GT7ShaderCacheCandidate -Root $Selection.shader_cache_source)) {
            throw 'Selected shader cache is no longer an inactive, nonempty and safe cache source.'
        }
    }
    $driverFiles = @()
    if ($null -ne $Selection.driver_cache_source) {
        if (($Selection.driver_cache_selection -eq 'completed_playtest' -and
             $null -eq (Get-GT7CompletedProgressCandidate -Root $Selection.driver_cache_source)) -or
            ($Selection.driver_cache_selection -eq 'seed_fallback' -and
             (Test-Path -LiteralPath (Join-Path $Selection.driver_cache_source 'playtest-manifest.json'))) -or
            -not (Test-GT7DriverCacheCandidate -Root $Selection.driver_cache_source)) {
            throw 'Selected driver cache is no longer an inactive, nonempty and safe cache source.'
        }
        $driverFiles = @(Get-GT7DriverCacheFiles -Root $Selection.driver_cache_source | Sort-Object Name)
        $relativeFiles = @($driverFiles | ForEach-Object { Join-Path '_PipelineCache' $_.Name })
        if (($relativeFiles -join '|') -cne ($Selection.driver_cache_relative_files -join '|')) {
            throw 'Selected driver cache files changed after selection.'
        }
    }
    $guard = $null
    try {
        if ($null -ne $Selection.save_path) {
            Assert-GT7StatePathNoReparse -LiteralPath $Selection.save_path
            # Prevent writes/renames of the chosen save throughout its copy.
            $guard = [IO.File]::Open($Selection.save_path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            if ((Get-FileHash -LiteralPath $Selection.save_path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Selection.save_sha256) {
                throw 'Selected progress changed after selection; refusing an inconsistent copy.'
            }
        }
        foreach ($relative in @('_SaveData\PPSA01317', '_Trophies\PPSA01317', '_DownloadData\PPSA01317', '_ShaderCache\PPSA01317')) {
            $fromRoot = $(if ($relative -eq '_ShaderCache\PPSA01317') { $Selection.shader_cache_source } else { $Selection.selected_root })
            if ($null -eq $fromRoot) { continue }
            $from = Join-Path $fromRoot $relative
            if (-not (Test-Path -LiteralPath $from -PathType Container)) { continue }
            Assert-GT7StateTreeNoReparse -LiteralPath $from
            $to = Join-Path $DestinationRoot $relative
            if (Test-Path -LiteralPath $to) { throw "Refusing to overwrite existing state: $to" }
            [void][IO.Directory]::CreateDirectory((Split-Path -Parent $to))
            Copy-Item -LiteralPath $from -Destination $to -Recurse -Force -ErrorAction Stop
        }
        if ($driverFiles.Count) {
            $to = Join-Path $DestinationRoot '_PipelineCache'
            if (Test-Path -LiteralPath $to) { throw "Refusing to overwrite existing state: $to" }
            [void][IO.Directory]::CreateDirectory($to)
            foreach ($file in $driverFiles) {
                Assert-GT7StatePathNoReparse -LiteralPath $file.FullName
                Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $to $file.Name) -ErrorAction Stop
            }
        }
        if ($null -ne $Selection.save_path) {
            $copied = Join-Path $DestinationRoot '_DownloadData\PPSA01317\gt7save.bin'
            if ((Get-FileHash -LiteralPath $copied -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Selection.save_sha256) {
                throw 'Copied GT7 save does not match the selected progress.'
            }
        }
    } finally {
        if ($null -ne $guard) { $guard.Dispose() }
    }
}
