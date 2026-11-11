<#
.SYNOPSIS
  Benchmarks Forge against Bun and Node.

.DESCRIPTION
  Runs every benchmark script in this folder on whichever of forge.exe,
  bun.exe and node.exe it finds. Each script is run once untimed (to check
  it works and to warm the file cache), then -Iterations times.

  Two numbers are recorded per run:

  * Total time: wall-clock time around the whole process, measured here.
    Includes process startup, engine start-up and shutdown.
  * Work time: the time the script itself measures around its workload
    with Date.now() and prints as "work-ms=<n>" (millisecond resolution).
    Excludes startup, so it compares how fast each runtime does the work.

  The comparison table uses work-time medians. Startup is compared on its
  own with startup.js. Before 2026-11-11 only total time existed, and
  Forge's ~30 ms faster startup decided the short benchmarks; see
  Forge_Benchmark_Report.md.

  Also records the machine and runtime versions next to the results, in
  bench/results/<timestamp>.env.txt.

.EXAMPLE
  .\bench\run-benchmarks.ps1
  .\bench\run-benchmarks.ps1 -ForgePath C:\Forge\bin\forge.exe -Iterations 7
#>
param(
    [string]$ForgePath = "C:\Forge\bin\forge.exe",
    [int]$Iterations = 7
)

# No $ErrorActionPreference = "Stop": in Windows PowerShell 5.1 that turns
# any line a runtime writes to stderr (captured with 2>&1) into a fatal error.
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Benchmarks = @(
    "startup.js",
    "json-bench.js",
    "loop-bench.js",
    "timer-bench.js",
    "microtask-bench.js",
    "promise-chain-bench.js",
    "fs-bench.js",
    "fs-async-bench.js"
)

function Resolve-Runtime {
    param([string]$CommandName, [string]$ExplicitPath)
    if ($ExplicitPath -and (Test-Path $ExplicitPath)) { return $ExplicitPath }
    $cmd = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Get-Median([double[]]$values) {
    $sorted = $values | Sort-Object
    $mid = [math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2 -eq 0) { return ($sorted[$mid - 1] + $sorted[$mid]) / 2 }
    return $sorted[$mid]
}

$Runtimes = [ordered]@{
    "forge" = Resolve-Runtime "forge.exe" $ForgePath
    "bun"   = Resolve-Runtime "bun.exe" $null
    "node"  = Resolve-Runtime "node.exe" $null
}

# --- Environment -----------------------------------------------------------
$envLines = New-Object System.Collections.Generic.List[string]
$envLines.Add("Date: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')")
try {
    $os = Get-CimInstance Win32_OperatingSystem
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $envLines.Add("OS: $($os.Caption) $($os.Version) (build $($os.BuildNumber))")
    $envLines.Add("CPU: $($cpu.Name.Trim()) ($($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) threads)")
    $envLines.Add("RAM: $([math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB")
} catch {
    $envLines.Add("OS/CPU: could not be read ($($_.Exception.Message))")
}
try { $envLines.Add("Power plan: $((powercfg /getactivescheme) -join ' ')") } catch { }
foreach ($key in $Runtimes.Keys) {
    $path = $Runtimes[$key]
    if (-not $path) { $envLines.Add("${key}: NOT FOUND (skipped)"); continue }
    $version = (& $path --version 2>&1 | Select-Object -First 1)
    $envLines.Add("${key}: $version ($path)")
}
$envLines.Add("Iterations: $Iterations (after one untimed warm-up run)")

Write-Host "Environment:"
$envLines | ForEach-Object { Write-Host "  $_" }
Write-Host ""

# --- Runs ------------------------------------------------------------------
$Results = New-Object System.Collections.Generic.List[object]

# The fs benchmarks write a scratch folder into the current directory.
# Forge denies file access by default (Kavach, Phase 8.2), so it gets
# read and write permission for exactly those two folders.
$RuntimeArgs = @{
    "forge" = @(
        "--allow-read=fs-bench-scratch", "--allow-write=fs-bench-scratch",
        "--allow-read=fs-async-bench-scratch", "--allow-write=fs-async-bench-scratch")
    "bun"   = @()
    "node"  = @()
}

Push-Location $ScriptDir
try {
    foreach ($bench in $Benchmarks) {
        $scriptPath = Join-Path $ScriptDir $bench
        if (-not (Test-Path $scriptPath)) { Write-Warning "Missing benchmark script: $scriptPath"; continue }

        foreach ($runtimeName in $Runtimes.Keys) {
            $exe = $Runtimes[$runtimeName]
            if (-not $exe) { continue }

            $extraArgs = $RuntimeArgs[$runtimeName]
            $output = & $exe @extraArgs $scriptPath 2>&1
            if ($LASTEXITCODE -ne 0 -or ($output -join "`n") -match "FAILED") {
                Write-Host ("{0,-24} {1,-6} FAILED (exit {2}): {3}" -f $bench, $runtimeName, $LASTEXITCODE, ($output -join ' '))
                continue
            }

            $totals = New-Object System.Collections.Generic.List[double]
            $works = New-Object System.Collections.Generic.List[double]
            for ($i = 0; $i -lt $Iterations; $i++) {
                $sw = [System.Diagnostics.Stopwatch]::StartNew()
                $out = & $exe @extraArgs $scriptPath 2>&1
                $sw.Stop()
                $totals.Add($sw.Elapsed.TotalMilliseconds)
                $m = [regex]::Match(($out -join "`n"), "work-ms=(\d+)")
                if ($m.Success) { $works.Add([double]$m.Groups[1].Value) }
            }

            $row = [PSCustomObject]@{
                Benchmark    = $bench
                Runtime      = $runtimeName
                TotalAvgMs   = [math]::Round(($totals | Measure-Object -Average).Average, 2)
                TotalMedianMs = [math]::Round((Get-Median $totals), 2)
                TotalMinMs   = [math]::Round(($totals | Measure-Object -Minimum).Minimum, 2)
                WorkMedianMs = if ($works.Count -gt 0) { [math]::Round((Get-Median $works), 1) } else { $null }
                WorkMinMs    = if ($works.Count -gt 0) { [math]::Round(($works | Measure-Object -Minimum).Minimum, 1) } else { $null }
            }
            $Results.Add($row)

            Write-Host ("{0,-24} {1,-6} work median={2,9} ms   total median={3,10:N2} ms   total min={4,10:N2} ms" -f `
                $bench, $runtimeName, $row.WorkMedianMs, $row.TotalMedianMs, $row.TotalMinMs)
        }
        Write-Host ""
    }
} finally {
    Pop-Location
}

# --- Comparison --------------------------------------------------------------
function Get-Row($bench, $runtime) {
    return $Results | Where-Object { $_.Benchmark -eq $bench -and $_.Runtime -eq $runtime }
}

Write-Host "=== Startup (startup.js total time, median; lower is better) ==="
foreach ($runtimeName in $Runtimes.Keys) {
    $r = Get-Row "startup.js" $runtimeName
    if ($r) { Write-Host ("  {0,-6} {1,10:N2} ms" -f $runtimeName, $r.TotalMedianMs) }
}
Write-Host ""

Write-Host "=== Work time, Forge vs Bun / Node (ratio < 1.0 means Forge is faster) ==="
foreach ($bench in $Benchmarks) {
    if ($bench -eq "startup.js") { continue }
    $f = Get-Row $bench "forge"
    foreach ($other in @("bun", "node")) {
        $o = Get-Row $bench $other
        if ($f -and $o -and $f.WorkMedianMs -and $o.WorkMedianMs) {
            Write-Host ("{0,-24} forge/{1,-4} = {2:N2}x   ({3} ms vs {4} ms)" -f $bench, $other, ($f.WorkMedianMs / $o.WorkMedianMs), $f.WorkMedianMs, $o.WorkMedianMs)
        }
    }
}
Write-Host ""

# --- Save --------------------------------------------------------------------
$LogDir = Join-Path $ScriptDir "results"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Timestamp = Get-Date -Format "yyyy-MM-dd_HHmmss"
$LogPath = Join-Path $LogDir "$Timestamp.csv"
$Results | Export-Csv -Path $LogPath -NoTypeInformation
$envLines | Set-Content -Path (Join-Path $LogDir "$Timestamp.env.txt")

Write-Host "Results saved to $LogPath (environment in $Timestamp.env.txt)"
