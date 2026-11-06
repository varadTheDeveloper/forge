<#
Benchmark runner — Forge vs Bun vs Node.

Originally written for the Phase 0 baseline (startup.js/json-bench.js/
loop-bench.js) and confirmed working end-to-end by the user at that
point (see bench/results/*.csv for those runs). Extended after Phase 6
(Runtime Integration) with three new scripts exercising the JS-visible
surface that phase actually changed: timer-bench.js (setTimeout,
backed by Phase 6's HashMap<int, UniquePtr<JsTimer>> timer registry),
microtask-bench.js (queueMicrotask, backed by Phase 6's
Queue<UniquePtr<Microtask>> microtask queue), and
promise-chain-bench.js (sequential Promise.then() chaining, exercising
the same microtask queue one continuation at a time rather than as one
flat burst). This second batch has not yet been run end-to-end — please
report back if anything errors out.

Extended again after Phase 7.3 (the `fs.*Sync` JS bindings, now
real-build-confirmed -- see Fs.md/HISTORY.md) with fs-bench.js, exercising
readFileSync/writeFileSync/appendFileSync/existsSync/mkdirSync/rmSync/
statSync -- the first script able to close the "filesystem operations"
gap the note below used to flag as not-yet-benchmarkable.

Extended again after Phase 7.5 (the Promise-returning `fs.*` JS bindings,
now real-build-confirmed through 7.5.5 -- see FsAsync.md/HISTORY.md) with
fs-async-bench.js, the async counterpart to fs-bench.js: same iteration
counts and payload, chained through .then() one step at a time (see that
script's own header comment for why concurrent calls over the same path
would be a genuine race here, not just a style choice) rather than
fs-bench.js's plain blocking for-loop. This is the first script in this
suite whose entire body of work runs after the script itself finishes
evaluating -- Runtime::run() has to actually drive it to completion --
so it also exercises the same event-loop-lifetime path Phase 7.5.5's
self-test harness isolates directly, as a byproduct rather than its main
purpose. Has not yet been run end-to-end — please report back what it
prints, same as every other script noted above.

Usage:
    .\run-benchmarks.ps1
    .\run-benchmarks.ps1 -ForgePath "C:\Forge\bin\forge.exe" -Iterations 7

Runs every .js benchmark script in this folder against whichever of
forge.exe / bun.exe / node.exe it can find, times each with
Measure-Command (wall-clock around the whole process, matching how the
architecture roadmap wants cold-start/throughput compared), and prints +
logs a results table so progress is a number you can watch move, not a
feeling.

Note on scope: this script covers what's actually JS-visible in Forge
today (print/setTimeout/setInterval/clearTimeout/queueMicrotask, the
fs.*Sync and Promise-returning fs.* surfaces added in Phase 7.3/7.5,
plus whatever engine-native globals SpiderMonkey provides, like JSON and
Promise). Forge Core's Socket/Thread (Phases 4-5) are still C++-only --
nothing in forge.cpp has wired them up as JS-callable functions yet --
so there is deliberately no networking or threading benchmark here;
Node/Bun have net/worker_threads bindings to compare against, Forge
doesn't yet have anything to point them at. (File was the exception:
Phase 7.3 wired up the sync surface, Phase 7.5 the async one, so
fs-bench.js/fs-async-bench.js now cover both.)
#>

param(
    [string]$ForgePath = "C:\Forge\bin\forge.exe",
    [int]$Iterations = 5
)

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

    if ($ExplicitPath -and (Test-Path $ExplicitPath)) {
        return $ExplicitPath
    }

    $cmd = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    return $null
}

$Runtimes = [ordered]@{
    "forge" = Resolve-Runtime "forge.exe" $ForgePath
    "bun"   = Resolve-Runtime "bun.exe" $null
    "node"  = Resolve-Runtime "node.exe" $null
}

Write-Host "Detected runtimes:"
foreach ($key in $Runtimes.Keys) {
    $path = $Runtimes[$key]
    if ($path) {
        Write-Host ("  {0,-6} -> {1}" -f $key, $path)
    } else {
        Write-Host ("  {0,-6} -> NOT FOUND (skipping)" -f $key)
    }
}
Write-Host ""

$Results = New-Object System.Collections.Generic.List[object]

foreach ($bench in $Benchmarks) {
    $scriptPath = Join-Path $ScriptDir $bench
    if (-not (Test-Path $scriptPath)) {
        Write-Warning "Missing benchmark script: $scriptPath"
        continue
    }

    foreach ($runtimeName in $Runtimes.Keys) {
        $exe = $Runtimes[$runtimeName]
        if (-not $exe) {
            continue
        }

        # One untimed run first: confirms the script actually runs and
        # prints the expected marker, and warms the OS file cache so run
        # #1 of the timed loop below isn't unfairly penalized.
        $output = & $exe $scriptPath 2>&1
        $exitCode = $LASTEXITCODE

        if ($exitCode -ne 0) {
            Write-Host ("{0,-24} {1,-6} FAILED (exit {2}): {3}" -f $bench, $runtimeName, $exitCode, ($output -join ' '))
            continue
        }

        $times = New-Object System.Collections.Generic.List[double]
        for ($i = 0; $i -lt $Iterations; $i++) {
            $elapsed = Measure-Command { & $exe $scriptPath *> $null }
            $times.Add($elapsed.TotalMilliseconds)
        }

        $avg = ($times | Measure-Object -Average).Average
        $min = ($times | Measure-Object -Minimum).Minimum

        # Median alongside avg/min: avg is easily skewed by one slow outlier
        # run (a stray GC pause, OS scheduling hiccup, etc.), especially at
        # the default -Iterations 5 -- median is a steadier number to
        # actually compare runtimes on.
        $sorted = $times | Sort-Object
        $mid = [math]::Floor($sorted.Count / 2)
        if ($sorted.Count % 2 -eq 0) {
            $median = ($sorted[$mid - 1] + $sorted[$mid]) / 2
        } else {
            $median = $sorted[$mid]
        }

        $Results.Add([PSCustomObject]@{
            Benchmark = $bench
            Runtime   = $runtimeName
            AvgMs     = [math]::Round($avg, 2)
            MedianMs  = [math]::Round($median, 2)
            MinMs     = [math]::Round($min, 2)
        })

        Write-Host ("{0,-24} {1,-6} avg={2,10:N2} ms   median={3,10:N2} ms   min={4,10:N2} ms" -f $bench, $runtimeName, $avg, $median, $min)
    }
    Write-Host ""
}

Write-Host "=== Forge vs Bun / Node (ratio < 1.0 means Forge is faster; based on MedianMs) ==="
foreach ($bench in $Benchmarks) {
    $forgeRow = $Results | Where-Object { $_.Benchmark -eq $bench -and $_.Runtime -eq "forge" }
    $bunRow   = $Results | Where-Object { $_.Benchmark -eq $bench -and $_.Runtime -eq "bun" }
    $nodeRow  = $Results | Where-Object { $_.Benchmark -eq $bench -and $_.Runtime -eq "node" }

    if ($forgeRow -and $bunRow) {
        $ratio = $forgeRow.MedianMs / $bunRow.MedianMs
        Write-Host ("{0,-24} forge/bun  ratio = {1:N2}x" -f $bench, $ratio)
    }
    if ($forgeRow -and $nodeRow) {
        $ratio = $forgeRow.MedianMs / $nodeRow.MedianMs
        Write-Host ("{0,-24} forge/node ratio = {1:N2}x" -f $bench, $ratio)
    }
}
Write-Host ""

# Save a timestamped log so results are comparable across sessions as
# each roadmap phase lands, not just visible in this one terminal.
$LogDir = Join-Path $ScriptDir "results"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Timestamp = Get-Date -Format "yyyy-MM-dd_HHmmss"
$LogPath = Join-Path $LogDir "$Timestamp.csv"
$Results | Export-Csv -Path $LogPath -NoTypeInformation

Write-Host "Results saved to $LogPath"
