[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$RunDirectory,
    [string]$ReportPath
)
# Read-only observation of a recorded native PSSR run. This does not launch the
# emulator, change settings, copy saves, or infer resolution from launch options.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-UnsignedField($Fields, [string]$Name) {
    if (-not $Fields.ContainsKey($Name)) { return $null }
    [uint64]$value = 0
    if ([uint64]::TryParse($Fields[$Name], [ref]$value)) { return $value }
    return $null
}

function Read-Rectangle($Value) {
    if ($null -eq $Value -or $Value -notmatch '^(\d+),(\d+),(\d+)x(\d+)$') { return $null }
    return [pscustomobject]@{
        x = [uint64]$Matches[1]; y = [uint64]$Matches[2]
        width = [uint64]$Matches[3]; height = [uint64]$Matches[4]
    }
}

function Same-Rectangle($Left, $Right) {
    return $null -ne $Left -and $null -ne $Right -and
        $Left.x -eq $Right.x -and $Left.y -eq $Right.y -and
        $Left.width -eq $Right.width -and $Left.height -eq $Right.height
}

function Add-Count($Table, [string]$Key) {
    if (-not $Table.ContainsKey($Key)) { $Table[$Key] = 0 }
    $Table[$Key]++
}

$run = (Resolve-Path -LiteralPath $RunDirectory).ProviderPath
$log = Join-Path $run 'stdout.log'
if (-not (Test-Path -LiteralPath $log -PathType Leaf)) { throw "Missing stdout.log: $log" }
$reportFile = $null
if ($ReportPath) {
    $reportFile = [IO.Path]::GetFullPath($ReportPath)
    # A report is the only permitted write. Never replace inputs or any file
    # inside a run's save/cache directory, even when given an accidental path.
    $runPrefix = $run.TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
    if ($reportFile.StartsWith($runPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw '-ReportPath must be outside RunDirectory; run evidence and saves are read-only.'
    }
    if (-not (Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($reportFile)) -PathType Container)) {
        throw '-ReportPath parent directory does not exist.'
    }
}

# Capture the bytes present at open time. Sharing permits inspection of a live
# log, but a live/truncated tail remains incomplete evidence, not an error-free run.
$stream = [IO.File]::Open($log, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                         [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
try {
    if ($stream.Length -gt [int]::MaxValue) { throw 'Log is too large for a diagnostic snapshot.' }
    $bytes = [byte[]]::new([int]$stream.Length)
    $length = 0
    while ($length -lt $bytes.Length) {
        $read = $stream.Read($bytes, $length, $bytes.Length - $length)
        if ($read -eq 0) { break }
        $length += $read
    }
    if ($length -ne $bytes.Length) { [Array]::Resize([ref]$bytes, $length) }
} finally { $stream.Dispose() }
$sha = [Security.Cryptography.SHA256]::Create()
try { $digest = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
finally { $sha.Dispose() }
$text = [Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xfeff)
$sequences = @{}
$bindings = @{}
$counters = [Collections.Generic.List[object]]::new()
$unparsedDiagnosticLines = 0
$fallbackLogLines = 0
$lineNumber = 0
foreach ($line in ($text -split '\r?\n')) {
    $lineNumber++
    if (-not $line.StartsWith('PSSR diagnostic:') -and
        -not $line.StartsWith('PSSR DLSS: requests=') -and
        -not $line.StartsWith('PSSR linear fallback:')) { continue }
    $fields = @{}
    foreach ($match in [regex]::Matches($line, '(\w+)=([^\s]+)')) {
        $fields[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    if (-not $line.StartsWith('PSSR diagnostic:')) {
        if ($line.StartsWith('PSSR linear fallback:')) { $fallbackLogLines++ }
        $requests = Read-UnsignedField $fields 'requests'
        $dlss = Read-UnsignedField $fields 'dlss'
        $linear = Read-UnsignedField $fields 'linear'
        if ($null -ne $requests -and $null -ne $dlss -and $null -ne $linear) {
            $counters.Add([pscustomobject]@{
                line = $lineNumber; requests = $requests; dlss = $dlss; linear = $linear
                input = $fields['input']; output = $fields['output']; reason = $fields['reason']
            })
        }
        continue
    }
    $seq = Read-UnsignedField $fields 'seq'
    if ($null -eq $seq) { $unparsedDiagnosticLines++; continue }
    $key = [string]$seq
    if (-not $sequences.ContainsKey($key)) {
        $sequences[$key] = @{
            seq = $seq; contexts = @{}; metadata = $null; metadata_count = 0
            input = $null; input_count = 0; dlss = $null; dlss_count = 0
            linear = $null; linear_count = 0; binding_count = 0
        }
    }
    $item = $sequences[$key]
    if ($fields.ContainsKey('context')) { $item.contexts[$fields['context']] = $true }
    if ($fields.ContainsKey('active')) {
        $item.metadata_count++
        $item.metadata = Read-Rectangle $fields['active']
    } elseif ($fields.ContainsKey('role') -and $fields.ContainsKey('grid')) {
        $role = switch ($fields['role']) {
            'input' { 'input' }
            'output-dlss' { 'dlss' }
            'output-linear' { 'linear' }
            default { $null }
        }
        if ($null -eq $role) { $unparsedDiagnosticLines++; continue }
        $item[$role + '_count']++
        $item[$role] = [pscustomobject]@{
            rect = Read-Rectangle $fields['rect']
            finite = Read-UnsignedField $fields 'finite'
            nonfinite = Read-UnsignedField $fields 'nonfinite'
            line = $lineNumber
        }
    } elseif ($fields.ContainsKey('role') -and $fields.ContainsKey('backing')) {
        $item.binding_count++
        Add-Count $bindings ($fields['role'] + ':' + $fields['backing'])
    } else { $unparsedDiagnosticLines++ }
}

$sizes = @{}
$missingMetadata = [Collections.Generic.List[uint64]]::new()
$incomplete = [Collections.Generic.List[uint64]]::new()
$ambiguous = [Collections.Generic.List[uint64]]::new()
$rectMismatch = [Collections.Generic.List[uint64]]::new()
$unknownResolution = 0
$dlssPairs = 0; $linearPairs = 0; $classifiedPairs = 0
$targetPairs = 0; $targetFinitePairs = 0; $targetRequests = 0
$targetFirst = $null; $targetLast = $null
$knownNonfinite = [uint64]0; $unknownFinitePairs = 0
$metadataRecords = 0; $inputRecords = 0; $dlssRecords = 0; $linearRecords = 0
foreach ($item in ($sequences.Values | Sort-Object { $_.seq })) {
    $metadataRecords += $item.metadata_count
    $inputRecords += $item.input_count
    $dlssRecords += $item.dlss_count
    $linearRecords += $item.linear_count
    $active = $item.metadata
    if ($item.metadata_count -ne 1 -or $null -eq $active) { $missingMetadata.Add($item.seq) }
    if ($item.metadata_count -eq 1 -and $null -ne $active -and
        $active.width -eq 1920 -and $active.height -eq 1080) { $targetRequests++ }
    $hasInput = $item.input_count -eq 1
    $hasDlss = $item.dlss_count -eq 1
    $hasLinear = $item.linear_count -eq 1
    if ($item.metadata_count -gt 1 -or $item.input_count -gt 1 -or
        $item.dlss_count -gt 1 -or $item.linear_count -gt 1 -or
        ($item.dlss_count -gt 0 -and $item.linear_count -gt 0) -or $item.contexts.Count -gt 1) {
        $ambiguous.Add($item.seq)
        continue
    }
    if (-not $hasInput -or (-not $hasDlss -and -not $hasLinear)) {
        $incomplete.Add($item.seq)
        continue
    }
    if ($hasLinear) { $linearPairs++; continue }
    $dlssPairs++
    $finiteKnown = $null -ne $item.input.finite -and $null -ne $item.input.nonfinite -and
                   $null -ne $item.dlss.finite -and $null -ne $item.dlss.nonfinite
    if ($finiteKnown) { $knownNonfinite += $item.input.nonfinite + $item.dlss.nonfinite }
    else { $unknownFinitePairs++ }
    if ($item.metadata_count -ne 1 -or $null -eq $active -or
        $null -eq $item.input.rect -or $null -eq $item.dlss.rect) {
        $unknownResolution++
        continue
    }
    if (-not (Same-Rectangle $active $item.input.rect)) {
        $rectMismatch.Add($item.seq)
        $unknownResolution++
        continue
    }
    $classifiedPairs++
    $output = $item.dlss.rect
    Add-Count $sizes ('{0}x{1} -> {2}x{3}' -f $active.width, $active.height, $output.width, $output.height)
    if ($active.width -eq 1920 -and $active.height -eq 1080 -and
        $output.width -eq 3840 -and $output.height -eq 2160) {
        $targetPairs++
        if ($null -eq $targetFirst) { $targetFirst = $item.seq }
        $targetLast = $item.seq
        if ($finiteKnown -and $item.input.finite -gt 0 -and $item.dlss.finite -gt 0 -and
            $item.input.nonfinite -eq 0 -and $item.dlss.nonfinite -eq 0) { $targetFinitePairs++ }
    }
}
$counterRegressions = 0
$lastCounter = $null
$maxRequests = $null; $maxDlss = $null; $maxLinear = $null
foreach ($counter in $counters) {
    if ($null -ne $lastCounter -and ($counter.requests -lt $lastCounter.requests -or
        $counter.dlss -lt $lastCounter.dlss -or $counter.linear -lt $lastCounter.linear)) { $counterRegressions++ }
    if ($null -eq $maxRequests -or $counter.requests -gt $maxRequests) { $maxRequests = $counter.requests }
    if ($null -eq $maxDlss -or $counter.dlss -gt $maxDlss) { $maxDlss = $counter.dlss }
    if ($null -eq $maxLinear -or $counter.linear -gt $maxLinear) { $maxLinear = $counter.linear }
    $lastCounter = $counter
}
$status = if ($targetPairs -gt 0) { 'observed_in_logged_pairs' }
          elseif ($classifiedPairs -gt 0) { 'not_observed_in_classified_pairs' }
          else { 'unknown_no_classifiable_pairs' }
$report = [ordered]@{
    schema = 1
    run_directory = $run
    source = [ordered]@{ path = $log; snapshot_bytes = $bytes.Length; sha256 = $digest
                        snapshot_utc = [DateTime]::UtcNow.ToString('o') }
    target = [ordered]@{
        input = '1920x1080'; output = '3840x2160'; status = $status
        metadata_requests_1080p = $targetRequests
        matched_native_dlss_pairs = $targetPairs; matched_finite_sample_pairs = $targetFinitePairs
        first_sequence = $targetFirst; last_sequence = $targetLast
    }
    diagnostic = [ordered]@{
        unique_sequences = $sequences.Count; metadata_records = $metadataRecords
        input_sample_records = $inputRecords; native_output_sample_records = $dlssRecords
        linear_output_sample_records = $linearRecords
        native_dlss_pairs = $dlssPairs; linear_pairs = $linearPairs
        resolution_classified_native_pairs = $classifiedPairs
        other_resolution_native_pairs = $classifiedPairs - $targetPairs
        unknown_resolution_native_pairs = $unknownResolution
        known_nonfinite_samples_in_native_pairs = $knownNonfinite
        native_pairs_missing_finite_statistics = $unknownFinitePairs
        missing_or_invalid_metadata_count = $missingMetadata.Count
        missing_or_invalid_metadata_first20 = @($missingMetadata | Select-Object -First 20)
        incomplete_pair_count = $incomplete.Count; incomplete_pair_first20 = @($incomplete | Select-Object -First 20)
        ambiguous_sequence_count = $ambiguous.Count; ambiguous_sequence_first20 = @($ambiguous | Select-Object -First 20)
        metadata_input_rectangle_mismatch_count = $rectMismatch.Count
        metadata_input_rectangle_mismatch_first20 = @($rectMismatch | Select-Object -First 20)
        unparsed_diagnostic_lines = $unparsedDiagnosticLines
    }
    resolution_pairs = @($sizes.Keys | Sort-Object | ForEach-Object {
        [pscustomobject]@{ resolution = $_; native_dlss_pairs = $sizes[$_] }
    })
    binding_backings = @($bindings.Keys | Sort-Object | ForEach-Object {
        [pscustomobject]@{ role_and_backing = $_; logged_bindings = $bindings[$_] }
    })
    cumulative_counters = [ordered]@{
        records = $counters.Count; latest = $lastCounter
        maximum_observed_requests = $maxRequests; maximum_observed_dlss = $maxDlss
        maximum_observed_linear = $maxLinear; regressions = $counterRegressions
        exact_run_totals_known = $false
    }
    fallback = [ordered]@{
        observed = $linearRecords -gt 0 -or ($null -ne $maxLinear -and $maxLinear -gt 0) -or $fallbackLogLines -gt 0
        logged_linear_outputs = $linearRecords; counter_maximum_linear = $maxLinear
        fallback_counter_log_lines = $fallbackLogLines
    }
    limitations = @(
        'A matched pair proves logged native DLSS used that active input/output rectangle; launch flags or presets are not execution evidence.',
        'Backing allocation may legitimately exceed the active input rectangle. Sparse binding sizes are informational and never used to reject a pair.',
        'These logs do not prove how the game produced the input, exclude an upstream crop, or validate final presentation, image quality or track-wide operation.',
        'Diagnostics are bounded and asynchronous. Missing fields, unmatched records, duplicate/context-conflicting sequences and an unfinished tail remain unknown.',
        'Periodic counters are observed cumulative lower bounds, not final totals; do not subtract or add them as per-resolution frame counts. Counter regressions may indicate concatenated runs.',
        'Finite statistics cover sampled points only. This report does not certify every pixel, an entire run, or gameplay acceptance.'
    )
}
$json = $report | ConvertTo-Json -Depth 10
if ($reportFile) { [IO.File]::WriteAllText($reportFile, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false)) }
else { $json }
