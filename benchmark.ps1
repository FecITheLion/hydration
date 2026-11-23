param(
    [switch]$SaveBaseline,
    [switch]$CompareBaseline,
    [string]$BaselinePath = ".\bench_baseline.json",
    [string]$HydrationExe = ".\hydration.exe",
    [string]$ValidatorExe = ".\test_validator.exe",
    [int]$Iterations = 7,
    [int]$Warmup = 1
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $HydrationExe)) {
    throw "Main binary not found: $HydrationExe"
}
if (-not (Test-Path $ValidatorExe)) {
    throw "Validator binary not found: $ValidatorExe"
}

$hydInfo = Get-Item (Resolve-Path $HydrationExe)
$valInfo = Get-Item (Resolve-Path $ValidatorExe)
$cpuInfo = Get-CimInstance Win32_Processor | Select-Object -First 1
$osInfo  = Get-CimInstance Win32_OperatingSystem

Write-Host ("=" * 100)
Write-Host " HYDRATION VERBOSE BENCHMARK & BOTTLENECK PROFILER"
Write-Host ("=" * 100)
Write-Host (" Timestamp     : {0}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Write-Host (" Host CPU      : {0} ({1} Cores / {2} Logical)" -f $cpuInfo.Name.Trim(), $cpuInfo.NumberOfCores, $cpuInfo.NumberOfLogicalProcessors)
Write-Host (" Host OS       : {0} (Build {1})" -f $osInfo.Caption.Trim(), $osInfo.BuildNumber)
Write-Host (" Main Binary   : {0} ({1} KB / {2:N0} bytes)" -f $hydInfo.FullName, [Math]::Round($hydInfo.Length / 1KB), $hydInfo.Length)
Write-Host (" Validator Bin : {0} ({1} KB / {2:N0} bytes)" -f $valInfo.FullName, [Math]::Round($valInfo.Length / 1KB), $valInfo.Length)
Write-Host ("=" * 100)
Write-Host ""

Write-Host -NoNewline "Synthesizing benchmark payloads in memory... "
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# 10k items
$sb10k = New-Object System.Text.StringBuilder
[void]$sb10k.Append('[')
for ($i = 0; $i -lt 10000; $i++) {
    if ($i -gt 0) { [void]$sb10k.Append(',') }
    [void]$sb10k.AppendFormat('"item_{0:D6}_val"', $i)
}
[void]$sb10k.Append(']')
$bytes10k = $utf8NoBom.GetBytes($sb10k.ToString())

# 25k escaped + zero-width strip
$sb25k = New-Object System.Text.StringBuilder
[void]$sb25k.Append('[')
for ($i = 0; $i -lt 25000; $i++) {
    if ($i -gt 0) { [void]$sb25k.Append(',') }
    [void]$sb25k.AppendFormat('"item\u005F{0:D6}\u200B\uFEFF_esc"', $i)
}
[void]$sb25k.Append(']')
$bytes25k = $utf8NoBom.GetBytes($sb25k.ToString())

# 50k Unicode fold + 50% dedup
$sb50k = New-Object System.Text.StringBuilder
[void]$sb50k.Append('[')
for ($i = 0; $i -lt 50000; $i++) {
    if ($i -gt 0) { [void]$sb50k.Append(',') }
    $id = $i % 25000
    [void]$sb50k.AppendFormat('"CAF\u00C9_{0:D6}_ITEM"', $id)
}
[void]$sb50k.Append(']')
$bytes50k = $utf8NoBom.GetBytes($sb50k.ToString())

# 100k stress
$sb100k = New-Object System.Text.StringBuilder
[void]$sb100k.Append('[')
for ($i = 0; $i -lt 100000; $i++) {
    if ($i -gt 0) { [void]$sb100k.Append(',') }
    [void]$sb100k.AppendFormat('"stress_payload_item_{0:D7}"', $i)
}
[void]$sb100k.Append(']')
$bytes100k = $utf8NoBom.GetBytes($sb100k.ToString())

Write-Host ("done. (10k={0:N0}B, 25kEsc={1:N0}B, 50kDedup={2:N0}B, 100k={3:N0}B)" -f $bytes10k.Length, $bytes25k.Length, $bytes50k.Length, $bytes100k.Length)
Write-Host ""

$benchDir = Join-Path $env:TEMP ("hydration_bench_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $benchDir -Force | Out-Null

function Get-Percentile([double[]]$sorted, [double]$pct) {
    if ($sorted.Count -eq 0) { return 0.0 }
    if ($sorted.Count -eq 1) { return $sorted[0] }
    $idx = ($pct / 100.0) * ($sorted.Count - 1)
    $lo = [Math]::Floor($idx)
    $hi = [Math]::Ceiling($idx)
    if ($lo -eq $hi) { return $sorted[$lo] }
    return $sorted[$lo] + ($idx - $lo) * ($sorted[$hi] - $sorted[$lo])
}

function Invoke-SingleRun {
    param(
        [string]$ExePath,
        [string]$Arguments,
        [byte[]]$StdinBytes = $null
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ExePath
    $psi.WorkingDirectory = $benchDir
    $psi.Arguments = $Arguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables["HYD_MANAGED"] = "1"

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)

    $outMem = New-Object System.IO.MemoryStream
    $outTask = $p.StandardOutput.BaseStream.CopyToAsync($outMem)
    $errTask = $p.StandardError.ReadToEndAsync()

    if ($null -ne $StdinBytes -and $StdinBytes.Length -gt 0) {
        $p.StandardInput.BaseStream.Write($StdinBytes, 0, $StdinBytes.Length)
    }
    $p.StandardInput.Close()

    $peakRss = 0L
    $commitBytes = 0L
    while (-not $p.HasExited) {
        try {
            $p.Refresh()
            if ($p.WorkingSet64 -gt $peakRss) { $peakRss = $p.WorkingSet64 }
            if ($p.PagedMemorySize64 -gt $commitBytes) { $commitBytes = $p.PagedMemorySize64 }
        } catch {}
        Start-Sleep -Milliseconds 1
    }

    $p.WaitForExit()
    $sw.Stop()
    $outTask.GetAwaiter().GetResult()
    $stderrText = $errTask.GetAwaiter().GetResult()

    try {
        $p.Refresh()
        if ($p.PeakWorkingSet64 -gt $peakRss) { $peakRss = $p.PeakWorkingSet64 }
        if ($p.PeakPagedMemorySize64 -gt $commitBytes) { $commitBytes = $p.PeakPagedMemorySize64 }
    } catch {}

    if ($p.ExitCode -ne 0) {
        throw "Process failed ($ExePath $Arguments) ExitCode=$($p.ExitCode): $stderrText"
    }

    [PSCustomObject]@{
        ElapsedMs   = $sw.Elapsed.TotalMilliseconds
        UserCpuMs   = $p.UserProcessorTime.TotalMilliseconds
        KernelCpuMs = $p.TotalProcessorTime.TotalMilliseconds - $p.UserProcessorTime.TotalMilliseconds
        PeakRssKB   = [Math]::Round($peakRss / 1KB)
        CommitKB    = [Math]::Round($commitBytes / 1KB)
        OutputBytes = $outMem.Length
    }
}

function Measure-Scenario {
    param(
        [string]$Name,
        [int]$ItemCount,
        [long]$InputBytes,
        [scriptblock]$SetupBlock = $null,
        [string]$ExePath = $hydInfo.FullName,
        [string]$Arguments,
        [byte[]]$StdinBytes = $null,
        [double]$SpawnFloorMs = 0.0
    )

    for ($w = 0; $w -lt $Warmup; $w++) {
        if ($null -ne $SetupBlock) { & $SetupBlock }
        [void](Invoke-SingleRun -ExePath $ExePath -Arguments $Arguments -StdinBytes $StdinBytes)
    }

    $runs = for ($i = 0; $i -lt $Iterations; $i++) {
        if ($null -ne $SetupBlock) { & $SetupBlock }
        Invoke-SingleRun -ExePath $ExePath -Arguments $Arguments -StdinBytes $StdinBytes
    }

    $latencies = @($runs | ForEach-Object { $_.ElapsedMs } | Sort-Object)
    $minMs = [Math]::Round($latencies[0], 2)
    $p50Ms = [Math]::Round((Get-Percentile $latencies 50), 2)
    $p95Ms = [Math]::Round((Get-Percentile $latencies 95), 2)
    $p99Ms = [Math]::Round((Get-Percentile $latencies 99), 2)
    $maxMs = [Math]::Round($latencies[-1], 2)

    $avgMs = ($latencies | Measure-Object -Average).Average
    $variance = 0.0
    foreach ($x in $latencies) { $variance += [Math]::Pow($x - $avgMs, 2) }
    $stdDevMs = [Math]::Round([Math]::Sqrt($variance / $latencies.Count), 2)
    $avgMsRounded = [Math]::Round($avgMs, 2)
    $jitterPct = if ($p50Ms -gt 0) { [Math]::Round(($stdDevMs / $p50Ms) * 100, 1) } else { 0.0 }

    $avgUserMs   = [Math]::Round((($runs | ForEach-Object { $_.UserCpuMs } | Measure-Object -Average).Average), 2)
    $avgKernelMs = [Math]::Round((($runs | ForEach-Object { $_.KernelCpuMs } | Measure-Object -Average).Average), 2)
    $netComputeMs = if ($SpawnFloorMs -gt 0) {
        [Math]::Round([Math]::Max(0.01, $avgMs - $SpawnFloorMs), 2)
    } else {
        $avgMsRounded
    }

    $cpuLoadPct = if ($avgMs -gt 0) { [Math]::Round((($avgUserMs + $avgKernelMs) / $avgMs) * 100, 1) } else { 0.0 }
    $grossIps = if ($ItemCount -gt 0 -and $p50Ms -gt 0) { [Math]::Round($ItemCount / ($p50Ms / 1000.0)) } else { 0 }
    $netIps   = if ($ItemCount -gt 0 -and $netComputeMs -gt 0) { [Math]::Round($ItemCount / ($netComputeMs / 1000.0)) } else { 0 }

    $maxRssKB    = ($runs | ForEach-Object { $_.PeakRssKB } | Measure-Object -Maximum).Maximum
    $maxCommitKB = ($runs | ForEach-Object { $_.CommitKB } | Measure-Object -Maximum).Maximum
    $outBytes    = $runs[-1].OutputBytes

    $bytesPerItem = if ($ItemCount -gt 0) { [Math]::Round(($maxRssKB * 1024.0) / $ItemCount, 1) } else { 0.0 }
    $rssRatio     = if ($InputBytes -gt 0) { [Math]::Round(($maxRssKB * 1024.0) / $InputBytes, 2) } else { 0.0 }

    [PSCustomObject]@{
        Scenario         = $Name
        MinMs            = $minMs
        P50Ms            = $p50Ms
        AvgMs            = $avgMsRounded
        P95Ms            = $p95Ms
        P99Ms            = $p99Ms
        MaxMs            = $maxMs
        StdDevMs         = $stdDevMs
        JitterPct        = $jitterPct
        NetComputeMs     = $netComputeMs
        UserCpuMs        = $avgUserMs
        KernelCpuMs      = $avgKernelMs
        CpuLoadPct       = $cpuLoadPct
        GrossItemsPerSec = $grossIps
        NetItemsPerSec   = $netIps
        PeakRss_KB       = $maxRssKB
        Commit_KB        = $maxCommitKB
        BytesPerItem     = $bytesPerItem
        RssPayloadRatio  = $rssRatio
        InputBytes       = $InputBytes
        OutputBytes      = $outBytes
    }
}

try {
    $results = New-Object System.Collections.Generic.List[object]

    # 1. Process Spawn Floor
    $r1 = Measure-Scenario -Name "1. Process Spawn Floor (--version)" -ItemCount 0 -InputBytes 0 -Arguments "--version"
    $results.Add($r1)
    $spawnFloor = $r1.AvgMs

    # 2. Cold Cache 10k
    $coldCache = Join-Path $benchDir "cold_10k.json"
    $r2 = Measure-Scenario -Name "2. Cold Cache 10k Stdin -> Disk+Out" -ItemCount 10000 -InputBytes $bytes10k.Length `
        -SetupBlock { Remove-Item $coldCache -Force -ErrorAction SilentlyContinue } `
        -Arguments '"@cold_10k.json"' -StdinBytes $bytes10k -SpawnFloorMs $spawnFloor
    $results.Add($r2)

    # 3. Warm Cache Hit 10k
    [System.IO.File]::WriteAllBytes((Join-Path $benchDir "warm_10k.json"), $bytes10k)
    $r3 = Measure-Scenario -Name "3. Warm Cache Hit 10k Disk -> Out" -ItemCount 10000 -InputBytes $bytes10k.Length `
        -Arguments '"@warm_10k.json"' -SpawnFloorMs $spawnFloor
    $results.Add($r3)

    # 4. 25k Escaped + Zero-Width Strip
    $escCache = Join-Path $benchDir "esc_25k.json"
    $r4 = Measure-Scenario -Name "4. 25k Escaped + Zero-Width Strip" -ItemCount 25000 -InputBytes $bytes25k.Length `
        -SetupBlock { Remove-Item $escCache -Force -ErrorAction SilentlyContinue } `
        -Arguments '"@esc_25k.json"' -StdinBytes $bytes25k -SpawnFloorMs $spawnFloor
    $results.Add($r4)

    # 5. 50k Unicode Fold + 50% Dedup (@file)
    $in50kFile = Join-Path $benchDir "in_50k.json"
    $out50kCache = Join-Path $benchDir "out_50k.json"
    [System.IO.File]::WriteAllBytes($in50kFile, $bytes50k)
    $r5 = Measure-Scenario -Name "5. 50k Unicode Fold + 50% Dedup (@file)" -ItemCount 50000 -InputBytes $bytes50k.Length `
        -SetupBlock { Remove-Item $out50kCache -Force -ErrorAction SilentlyContinue } `
        -Arguments '"@out_50k.json"' -StdinBytes ([System.IO.File]::ReadAllBytes($in50kFile)) -SpawnFloorMs $spawnFloor
    $results.Add($r5)

    # 6. 100k Stress
    $stressCache = Join-Path $benchDir "stress_100k.json"
    $r6 = Measure-Scenario -Name "6. 100k Items Stress (3.2MB Stdin)" -ItemCount 100000 -InputBytes $bytes100k.Length `
        -SetupBlock { Remove-Item $stressCache -Force -ErrorAction SilentlyContinue } `
        -Arguments '"@stress_100k.json"' -StdinBytes $bytes100k -SpawnFloorMs $spawnFloor
    $results.Add($r6)

    # 7. Native Validator
    $r7 = Measure-Scenario -Name "7. Native Validator (.exe CreateProcessW)" -ItemCount 0 -InputBytes 0 `
        -ExePath $valInfo.FullName -Arguments ('"{0}"' -f $hydInfo.FullName) -SpawnFloorMs $spawnFloor
    $results.Add($r7)

    Write-Host "=== [1] LATENCY DISTRIBUTION & JITTER (ms) ==="
    $results | Format-Table Scenario, MinMs, P50Ms, AvgMs, P95Ms, P99Ms, MaxMs, StdDevMs, JitterPct -AutoSize | Out-Host

    Write-Host "=== [2] CPU SPLIT, THROUGHPUT & BOTTLENECK ATTRIBUTION ==="
    $results | Format-Table Scenario, NetComputeMs, UserCpuMs, KernelCpuMs, CpuLoadPct, GrossItemsPerSec, NetItemsPerSec -AutoSize | Out-Host

    Write-Host "=== [3] MEMORY FOOTPRINT & ALLOCATION EFFICIENCY ==="
    $results | Format-Table Scenario, PeakRss_KB, Commit_KB, BytesPerItem, RssPayloadRatio, InputBytes, OutputBytes -AutoSize | Out-Host

    if ($SaveBaseline) {
        $payload = [PSCustomObject]@{
            SavedAt        = (Get-Date -Format "o")
            BinaryBytes    = $hydInfo.Length
            ValidatorBytes = $valInfo.Length
            Results        = $results.ToArray()
        }
        $jsonText = $payload | ConvertTo-Json -Depth 5
        [System.IO.File]::WriteAllText((Join-Path (Get-Location) $BaselinePath), $jsonText, $utf8NoBom)
        Write-Host ("[BASELINE SAVED] Wrote clean metrics to {0}" -f (Resolve-Path $BaselinePath)) -ForegroundColor Green
        Write-Host ""
    }

    if ($CompareBaseline) {
        Write-Host ("=== [4] BASELINE COMPARISON (vs. {0}) ===" -f (Resolve-Path $BaselinePath -ErrorAction SilentlyContinue))
        if (-not (Test-Path $BaselinePath)) {
            Write-Warning "Baseline file not found at '$BaselinePath'. Run '.\benchmark.ps1 -SaveBaseline' first."
        } else {
            $rawBase = Get-Content $BaselinePath -Raw | ConvertFrom-Json
            $baseItems = if ($null -ne $rawBase.Results) { @($rawBase.Results) } else { @($rawBase) }
            $baseMap = @{}
            foreach ($item in $baseItems) {
                if ($null -ne $item.Scenario -and $null -ne $item.P50Ms) {
                    $baseMap[$item.Scenario] = $item
                }
            }

            if ($baseMap.Count -eq 0) {
                Write-Warning "Baseline file '$BaselinePath' contains no valid scenario metrics (likely corrupted). Regenerate with '.\benchmark.ps1 -SaveBaseline'."
            } else {
                $deltas = foreach ($cur in $results) {
                    if ($baseMap.ContainsKey($cur.Scenario)) {
                        $b = $baseMap[$cur.Scenario]
                        $p50DiffMs  = [Math]::Round($cur.P50Ms - [double]$b.P50Ms, 2)
                        $p50DiffPct = if ([double]$b.P50Ms -gt 0) { [Math]::Round(($p50DiffMs / [double]$b.P50Ms) * 100, 1) } else { 0.0 }
                        $rssDiffKB  = [Math]::Round($cur.PeakRss_KB - [double]$b.PeakRss_KB)
                        $ipsDiffPct = if ([double]$b.NetItemsPerSec -gt 0) {
                            [Math]::Round((($cur.NetItemsPerSec - [double]$b.NetItemsPerSec) / [double]$b.NetItemsPerSec) * 100, 1)
                        } else { 0.0 }

                        [PSCustomObject]@{
                            Scenario       = $cur.Scenario
                            BaseP50Ms      = $b.P50Ms
                            CurrP50Ms      = $cur.P50Ms
                            P50DeltaMs     = $(if ($p50DiffMs -gt 0) { "+$p50DiffMs" } else { "$p50DiffMs" })
                            P50DeltaPct    = $(if ($p50DiffPct -gt 0) { "+$p50DiffPct%" } else { "$p50DiffPct%" })
                            BaseRssKB      = $b.PeakRss_KB
                            CurrRssKB      = $cur.PeakRss_KB
                            RssDeltaKB     = $(if ($rssDiffKB -gt 0) { "+$rssDiffKB" } else { "$rssDiffKB" })
                            NetIpsDeltaPct = $(if ($ipsDiffPct -gt 0) { "+$ipsDiffPct%" } else { "$ipsDiffPct%" })
                        }
                    }
                }
                $deltas | Format-Table Scenario, BaseP50Ms, CurrP50Ms, P50DeltaMs, P50DeltaPct, BaseRssKB, CurrRssKB, RssDeltaKB, NetIpsDeltaPct -AutoSize | Out-Host
            }
        }
    }
}
finally {
    Remove-Item $benchDir -Recurse -Force -ErrorAction SilentlyContinue
}