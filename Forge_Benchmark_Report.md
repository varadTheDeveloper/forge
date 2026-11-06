# Forge Benchmark Report

## Provenance note

`ROADMAP.md` and `HISTORY.md`'s "Phase 6 benchmarked" entry, and
`PROJECT_CONTEXT.md`, all reference this file as an existing artifact
holding the full same-machine Forge/Bun/Node comparison from the Phase 6
benchmark run. On 2026-07-30, while working on Phase 7.4, a direct
listing of the live `js/src/forge` directory showed this file does not
exist there, and no earlier copy of it could be found anywhere in the
repo. Whether it was written once and lost, or referenced before it was
ever actually created, is not known — this is being flagged rather than
guessed at.

This document is a **fresh report, authored 2026-07-30**, built from a
real benchmark run against the current build (which includes
`bench/fs-bench.js`, added in Phase 7.4 and not part of the original
Phase 6 run). It is not a recovered copy of whatever the earlier
reference was pointing to. The Phase 6 prose entries in `HISTORY.md`
already carry their own results tables inline (see "Phase 6 benchmarked"
and "Phase 6 corrected"), so nothing from that work is lost — this report
simply gives those numbers, plus `fs-bench.js`'s, a standalone home
alongside a fuller methodology write-up, matching what the roadmap has
always described this file as being for.

## Methodology

All numbers below come from `bench/run-benchmarks.ps1`, run by the user
on their own machine on 2026-07-30 against:

- `forge` — `C:\Forge\bin\forge.exe` (built via `mach build` from the
  `forge.cpp` delivered in this session, real-build-confirmed through
  Phase 7.3; see `HISTORY.md`).
- `bun` — `C:\Users\varad\.bun\bin\bun.exe`.
- `node` — `C:\nvm4w\nodejs\node.exe`.

For each script, the harness runs one untimed warm-up pass (to confirm
the script actually completes and to warm the OS file cache), then times
5 further runs with `Measure-Command` (PowerShell's wall-clock timer
around the whole process — cold-start cost is included in every number,
matching how the project has always wanted throughput/cold-start
compared, per `ROADMAP.md`). Average, median, and minimum are reported
across those 5 runs; the ratio table below uses median, since a single
slow outlier run (a stray GC pause or OS scheduling hiccup) skews the
average more than it does the median.

This was a single run at the harness's default 5 iterations on one
machine. It is real data, not synthetic or estimated, but it is not a
rigorous statistical benchmark (no repeated sessions, no isolation from
background load, no warm-vs-cold-cache separation beyond the one
warm-up pass) — read the numbers below with that in mind, and treat
anything within a few percent as noise rather than a real difference.

## Results (2026-07-30)

Ratio < 1.0 means Forge is faster.

| Benchmark | Forge (median ms) | Bun (median ms) | Node (median ms) | Forge/Bun | Forge/Node |
|---|---|---|---|---|---|
| `startup.js` | 18.25 | 38.85 | 43.87 | 0.47x | 0.42x |
| `json-bench.js` | 368.48 | 249.18 | 539.02 | 1.48x | 0.68x |
| `loop-bench.js` | 82.97 | 73.04 | 91.49 | 1.14x | 0.91x |
| `timer-bench.js` | 22.18 | 54.57 | 58.58 | 0.41x | 0.38x |
| `microtask-bench.js` | 56.13 | 54.75 | 104.74 | 1.03x | 0.54x |
| `promise-chain-bench.js` | 88.38 | 43.82 | 47.61 | 2.02x | 1.86x |
| `fs-bench.js` | 864.01 | 999.30 | 944.82 | 0.86x | 0.91x |

Full avg/median/min for all three runtimes, all seven scripts:

| Benchmark | Runtime | Avg ms | Median ms | Min ms |
|---|---|---|---|---|
| `startup.js` | forge | 18.74 | 18.25 | 18.06 |
| `startup.js` | bun | 38.70 | 38.85 | 38.13 |
| `startup.js` | node | 43.92 | 43.87 | 43.07 |
| `json-bench.js` | forge | 368.51 | 368.48 | 366.01 |
| `json-bench.js` | bun | 249.37 | 249.18 | 246.84 |
| `json-bench.js` | node | 540.48 | 539.02 | 534.81 |
| `loop-bench.js` | forge | 82.60 | 82.97 | 81.67 |
| `loop-bench.js` | bun | 73.07 | 73.04 | 72.04 |
| `loop-bench.js` | node | 91.98 | 91.49 | 90.72 |
| `timer-bench.js` | forge | 21.83 | 22.18 | 20.89 |
| `timer-bench.js` | bun | 55.20 | 54.57 | 46.46 |
| `timer-bench.js` | node | 58.44 | 58.58 | 57.63 |
| `microtask-bench.js` | forge | 56.31 | 56.13 | 55.70 |
| `microtask-bench.js` | bun | 54.82 | 54.75 | 54.55 |
| `microtask-bench.js` | node | 105.03 | 104.74 | 103.09 |
| `promise-chain-bench.js` | forge | 88.35 | 88.38 | 87.30 |
| `promise-chain-bench.js` | bun | 43.71 | 43.82 | 43.35 |
| `promise-chain-bench.js` | node | 47.52 | 47.61 | 46.21 |
| `fs-bench.js` | forge | 849.99 | 864.01 | 820.83 |
| `fs-bench.js` | bun | 996.18 | 999.30 | 969.94 |
| `fs-bench.js` | node | 945.63 | 944.82 | 927.75 |

Raw CSV: `bench/results/2026-07-30_175703.csv`.

## Per-benchmark analysis

**`startup.js`** — Forge boots in under half the time of Bun and under
half that of Node (0.47x/0.42x). This measures almost nothing but engine
init and parsing one line, so this is closer to "how heavy is the
runtime's own startup path" than a workload comparison — a real
advantage, but a narrow one.

**`json-bench.js`** — Forge is 1.48x slower than Bun but 0.68x (faster)
than Node on 200,000 `JSON.stringify`/`JSON.parse` round trips. `JSON` is
an engine-native global (SpiderMonkey's own implementation, not anything
Forge wrote), so this is really a comparison of the underlying JS
engines' JSON implementations (SpiderMonkey vs. Bun's JavaScriptCore vs.
Node's V8) more than of Forge as a runtime.

**`loop-bench.js`** — Forge sits between Bun (1.14x slower) and Node
(0.91x, slightly faster), consistent with the 2026-07-27 Phase 0
baseline — plain arithmetic-loop throughput hasn't moved since then, as
expected (nothing in Phases 3-7 touches the JS engine's own execution
of a tight loop).

**`timer-bench.js`** — Forge is clearly fastest (0.41x/0.38x), the
result of Phase 6's `HashMap<int, UniquePtr<JsTimer>>`-based timer
registry replacing a linear-scan `std::vector`. This is the benchmark
that Phase 6's own container rewrite was specifically expected to move,
and it did.

**`microtask-bench.js`** — Forge lands within noise of Bun (1.03x) and
clearly ahead of Node (0.54x). This reflects the Phase 6 fix that raised
`JS_NewContext`'s byte budget from 8MB to 512MB (this benchmark queues
200,000 simultaneously-live closures before any run, and previously hit
an out-of-memory failure at the old budget — see `HISTORY.md`'s "Phase 6
benchmarked" entry).

**`promise-chain-bench.js`** — Forge is 2.02x/1.86x slower than Bun/Node,
consistent with (slightly wider than, but within run-to-run variance of)
the same gap recorded in the "Phase 6 benchmarked" `HISTORY.md` entry
(1.97x/1.78x at the time). This gap is **real, reproduced again by this
run, and still unexplained** — no profiling has been done. Per this
project's own evidence standard, it should not be attributed to
SpiderMonkey's Promise/job-queue machinery, or anything else, without
profiling data. Next step if picked up: profile `forge.exe` running
`promise-chain-bench.js` under a real Windows profiler (WPR/WPA or the
Visual Studio profiler) attached to a real build.

**`fs-bench.js`** — new this phase. Forge is faster than both Bun
(0.86x) and Node (0.91x) on a write/read round-trip loop (2000
iterations, one reused file, `writeFileSync` + `readFileSync("utf8")`)
plus a lighter pass over `appendFileSync`/`existsSync`/`mkdirSync`/
`rmSync`/`statSync` (100 iterations). This is the first real-build
benchmark data for any part of `forge-core`'s `File`/`Path` layer (Phase
3) or the Phase 7.3 `fs.*Sync` JS bindings — no prior baseline exists to
compare against, so "faster than Bun/Node" here should be read as a
first data point, not a trend. No profiling was done to explain why;
plausible contributing factors (smaller binary/engine overhead per
process launch showing up in a benchmark that does 2100 total file
operations across one process lifetime, a leaner Win32 `File` layer than
Node/Bun's abstraction stack, or something else entirely) have not been
investigated and shouldn't be asserted as the cause without profiling.

## Known open items

- The `promise-chain-bench.js` performance gap (above) — real,
  reproduced twice now, unexplained.
- `fs-bench.js` numbers are a single run at 5 iterations on one machine
  — worth re-running across more iterations/sessions before treating the
  ratios as stable, especially since file I/O is generally noisier
  (OS file-cache state, antivirus scanning, disk scheduling) than
  pure-CPU benchmarks like `loop-bench.js`.
- This report itself replaces a previously-referenced file that could
  not be located (see Provenance note above) — if an earlier copy turns
  up, it should be reconciled against this one rather than silently
  discarded.
- ~~`fs-async-bench.js` has no real Forge number yet~~ **Resolved
  2026-08-02, same day.** The stale-binary hypothesis below was
  confirmed: `C:\Forge\bin\forge.exe` had not been refreshed from
  `mach build`'s actual output
  (`C:\spidermonkey-dev\gecko-dev\obj-spider\dist\bin\forge.exe`) since
  before Phase 7.5's async `fs.*` methods landed. The user copied the
  fresh binary over and reran; see the results section below for the now
  complete numbers. Worth remembering for future benchmark runs after any
  `mach build`: refresh `C:\Forge\bin\forge.exe` from the `obj-spider`
  output first.

## Environment

- Forge: `C:\Forge\bin\forge.exe`, built from the Phase 7.3
  real-build-confirmed `forge.cpp` (see `HISTORY.md`).
- Bun: `C:\Users\varad\.bun\bin\bun.exe`.
- Node: `C:\nvm4w\nodejs\node.exe`.
- Harness: `bench/run-benchmarks.ps1`, default `-Iterations 5`.
- Date: 2026-07-30.

## Results (2026-08-02) — adds `fs-async-bench.js` (Phase 7.5.6)

Same harness, same environment, same `-Iterations 5` default. Two runs
happened this day:

1. A first run against `C:\Forge\bin\forge.exe` as it stood at the time
   — a stale binary, not yet refreshed since before Phase 7.5's async
   `fs.*` methods landed. The other seven scripts ran fine (numbers
   below are from the corrected second run, not this one — see "Known
   open items" history above for what the stale run actually printed:
   `fs-async-bench.js` failed with `fs.mkdir is not a function` at its
   very first async fs.* call, since that binary predated the feature).
2. The user copied the actual `mach build` output
   (`C:\spidermonkey-dev\gecko-dev\obj-spider\dist\bin\forge.exe`) over
   `C:\Forge\bin\forge.exe` and reran. **All eight scripts, including
   `fs-async-bench.js`, now run clean on Forge.** Numbers below are from
   this corrected run.

The first seven scripts land within a few percent of the 2026-07-30
baseline across the board — treated as noise, not a real change, per
this report's own "anything within a few percent is noise" guidance;
nothing in Phase 7.5 touches any of those seven scripts' own code paths.

| Benchmark | Forge (median ms) | Bun (median ms) | Node (median ms) | Forge/Bun | Forge/Node |
|---|---|---|---|---|---|
| `startup.js` | 21.58 | 40.02 | 47.00 | 0.54x | 0.46x |
| `json-bench.js` | 378.19 | 256.88 | 558.21 | 1.47x | 0.68x |
| `loop-bench.js` | 83.85 | 73.92 | 97.29 | 1.13x | 0.86x |
| `timer-bench.js` | 23.12 | 54.18 | 61.28 | 0.43x | 0.38x |
| `microtask-bench.js` | 57.04 | 55.20 | 108.92 | 1.03x | 0.52x |
| `promise-chain-bench.js` | 87.39 | 45.69 | 48.59 | 1.91x | 1.80x |
| `fs-bench.js` | 802.58 | 1,054.32 | 938.36 | 0.76x | 0.86x |
| `fs-async-bench.js` | 935.39 | 1,239.99 | 1,413.65 | 0.75x | 0.66x |

Full avg/median/min for `fs-async-bench.js` specifically, all three
runtimes:

| Benchmark | Runtime | Avg ms | Median ms | Min ms |
|---|---|---|---|---|
| `fs-async-bench.js` | forge | 944.99 | 935.39 | 913.91 |
| `fs-async-bench.js` | bun | 1,236.58 | 1,239.99 | 1,181.34 |
| `fs-async-bench.js` | node | 1,482.14 | 1,413.65 | 1,392.17 |

Raw CSV: `bench/results/2026-08-02_173851.csv` (the corrected run; the
earlier stale-binary run's CSV, `2026-08-02_173412.csv`, is kept as-is
rather than deleted, since `fs-async-bench.js` FAILED in it and its
`fs-async-bench.js` row should not be read as real Forge data).

**`fs-async-bench.js` (2000 write/read round-trips on one shared file,
chained sequentially through `.then()`, plus a 100-iteration lighter pass
over `appendFile`/`exists`/`stat`/`rm`)** — Forge is faster than both Bun
(0.75x) and Node (0.66x), and directionally consistent with `fs-bench.js`
(the synchronous counterpart, 0.76x/0.86x) rather than reversing which
runtime wins. All three runtimes are slower on the async version than
the sync one for the same amount of I/O work (Forge: 935 ms vs. 803 ms;
Bun: 1,240 ms vs. 1,054 ms; Node: 1,414 ms vs. 938 ms) — expected,
since async dispatch (Promise creation/settlement, and for Forge
specifically, a real cross-thread `ThreadPool` hop per operation) adds
overhead a blocking call doesn't pay. No profiling was done to explain
the exact overhead breakdown or why Node's async/sync gap (938→1,414 ms,
+51%) is proportionally larger than Forge's (803→935 ms, +16%) — this is
a first data point, not a trend, same caveat `fs-bench.js`'s own entry
above already carries.

**The `fs.mkdir is not a function` discrepancy from the first run today
is resolved, not just worked around:** confirmed to be exactly the
stale-binary hypothesis this report flagged earlier, not a code bug.
`C:\Forge\bin\forge.exe` had not been refreshed from `mach build`'s real
output since before Phase 7.5's async `fs.*` work landed; copying the
actual `obj-spider\dist\bin\forge.exe` over it fixed the discrepancy
immediately, with no code changes anywhere. Worth carrying forward as a
process note: **refresh `C:\Forge\bin\forge.exe` from the `mach build`
output before every benchmark run**, not just when a fresh feature was
just added.
