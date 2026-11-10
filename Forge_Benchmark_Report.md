# Forge Benchmark Report

Forge compared with Bun and Node on one Windows x64 machine. The most recent
results come first; older runs are kept at the end as history.

**How to read these numbers.** Every script is a small, synthetic workload.
They show where Forge stands on a few specific operations, not how fast a
real application would be. Forge is also a much smaller runtime than Bun or
Node today (no module loader, no console object, almost no web APIs), which
is part of why it starts faster.

## Current results (2026-11-10, second run): work time measured in each script

Run by the user with `bench\run-benchmarks.ps1` (7 timed runs after one
warm-up). Raw data: `bench/results/2026-11-10_190436.csv`, environment in
`bench/results/2026-11-10_190436.env.txt`.

- Machine: AMD Ryzen 5 5600X (6 cores, 12 threads), 32 GB RAM, Windows 11 Pro
  build 26200, Balanced power plan.
- Forge: v0.1.0 (Phase 7.95 build, SpiderMonkey ESR 153.8). Bun 1.3.14.
  Node 24.11.1.

### Startup

Total time for `startup.js` (start, print one line, exit), median:

| Forge | Bun | Node |
|---:|---:|---:|
| **15.3 ms** | 40.4 ms | 43.1 ms |

Forge starts about 2.6x faster. It is also a much smaller runtime today, so
expect this gap to shrink as features are added.

### Work time

Measured by each script around its own workload (median of 7, milliseconds;
lower is better). Ratio < 1.0 means Forge is faster.

| Benchmark | Workload | Forge | Bun | Node | Forge/Bun | Forge/Node |
|---|---|---:|---:|---:|---:|---:|
| `json-bench.js` | 200,000 stringify+parse | 361 | **209** | 286 | 1.73x | 1.26x |
| `loop-bench.js` | `fib(36)` | 131 | **82** | 129 | 1.60x | 1.02x |
| `timer-bench.js` | 100,000 `setTimeout(fn, 0)` | 40 | 43 | **35** | 0.93x | 1.14x |
| `microtask-bench.js` | 1,000,000 `queueMicrotask` | 122 | **54** | 254 | 2.26x | 0.48x |
| `promise-chain-bench.js` | 1,000,000 chained `.then()` | 246 | 60 | **32** | 4.10x | 7.69x |
| `fs-bench.js` | 2,000 write+read, 100 misc | **1,609** | 1,908 | 1,674 | 0.84x | 0.96x |
| `fs-async-bench.js` | the same, Promise-based | **1,683** | 1,946 | 2,050 | 0.86x | 0.82x |

### What this run shows

- **Startup and file I/O are where Forge leads.** Async file I/O is 14-18%
  faster than Bun and Node. Sync file I/O beats Bun by 16% but Node by only
  4%, which is close to the noise.
- **Timers are roughly level** (35-43 ms for 100,000). Earlier reports said
  Forge was 3x faster; that was startup time, not timers.
- **Microtasks:** Bun is 2.3x faster than Forge; Forge is 2.1x faster than
  Node.
- **JSON and the recursive loop** mostly measure the JS engines themselves
  (SpiderMonkey against JavaScriptCore and V8). Bun leads both; Forge and
  Node tie on the loop.
- **Promise chains are Forge's biggest gap:** about 246 ns per step against
  60 ns for Bun and 32 ns for Node. The earlier startup-subtracted estimate
  (3.4x against Bun, 2.7x against Node) understated it, mostly for Node:
  with a million steps instead of 50,000, V8's optimising compiler has time
  to warm up. This is the main throughput target. Next step: profile
  `forge.exe` on this script (Windows Performance Recorder) to see whether
  the time goes to the engine's Promise machinery or to Forge's job-queue
  drain loop.
- **File I/O was about 60% slower for all three runtimes than in the first
  run of the day** (Forge 1,024 to 1,609 ms, Bun 1,160 to 1,908, Node 1,046
  to 1,674). The harness now runs from `C:\Forge\src\bench`, so the scratch
  files moved to a different folder; Windows Defender scanning that folder
  is the likely cause. The comparison between runtimes stays valid because
  all three ran in the same place, but absolute file-I/O times should only
  be compared within one run.

### Open items

- Profile the promise-chain gap (above).
- Check the file-I/O folder effect: rerun with the scratch folder in a
  Defender-excluded location, or confirm with Defender's exclusion list.
- Profile `File::ReadAllBytes` and the Phase 7.9 bounds checks for the
  sync file I/O shift noted in the first run.

## How the benchmarks work

`bench/run-benchmarks.ps1` runs every script in `bench/` on `forge.exe`,
`bun.exe` and `node.exe`: one untimed warm-up run, then 7 timed runs
(`-Iterations`). Since 2026-11-11 it records two numbers per run:

- **Total time:** wall-clock time around the whole process, measured by the
  harness. Includes process startup and shutdown.
- **Work time:** measured by the script itself with `Date.now()` around its
  workload, printed as `work-ms=<n>` (millisecond resolution). Excludes
  startup. The comparison table uses work-time medians; startup is compared
  separately with `startup.js`.

It also saves the OS, CPU, RAM, power plan and the three runtime versions in
`bench/results/<timestamp>.env.txt`, next to the CSV.

| Script | Workload (since 2026-11-11) | Before 2026-11-11 |
|---|---|---|
| `startup.js` | print one line | same |
| `json-bench.js` | 200,000 `JSON.stringify` + `JSON.parse` round trips | same |
| `loop-bench.js` | recursive `fib(36)` | `fib(34)` |
| `timer-bench.js` | 100,000 `setTimeout(fn, 0)` | 5,000 |
| `microtask-bench.js` | 1,000,000 `queueMicrotask` callbacks | 200,000 |
| `promise-chain-bench.js` | 1,000,000 chained `.then()` steps | 50,000 |
| `fs-bench.js` | 2,000 write+read round trips, plus 100 append/exists/stat/rm passes | same |
| `fs-async-bench.js` | the same as `fs-bench.js`, through the Promise-based `fs.*` | same |

The larger workloads make the work take long enough (tens to hundreds of
milliseconds) that timer resolution and noise matter less. Results from
before 2026-11-11 for the changed scripts are not comparable with later ones.

Known limits: one machine, no isolation from background load, file-cache
state not controlled, and `Date.now()` only has millisecond resolution.
Treat differences of a few percent as noise.

## History (superseded runs)

These are the earlier reports, kept as they were written, with headings
moved down one level. Their tables are real measurements, but **some of
their conclusions no longer hold**:

- **Every ratio in these sections includes process startup.** Forge starts
  about 20-30 ms sooner than Bun and Node, which by itself decides the short
  benchmarks (startup, timers, microtasks, promise chains), so "Forge is
  faster" in those rows mostly means "Forge starts faster". See the
  startup-subtracted view in "Current results".
- **The promise-chain gap is no longer "unexplained".** Most of it was async
  stack capture on every Promise, turned off by default in Phase 7.9 (a 96 ms
  to 63 ms total on the 142 engine), and the move to ESR 153 helped too
  (63 ms to 42 ms). Forge is still about 3.4x slower than Bun per step.
- **The microtask analysis credits a 512 MB heap budget.** Phase 7.9 replaced
  that fixed budget with real GC settings; the explanation no longer applies.
- **The "Known open items" and "Environment" subsections are out of date.**
  The current open items are listed under "Current results".
- **"Forge is 3x faster at timers" was startup.** With work time measured
  directly (second run of 2026-11-10), timers are roughly level with Bun and
  Node.
- A short provenance note about an earlier, lost version of this report
  was removed (2026-11-11); `HISTORY.md` keeps the story.

### Results (2026-11-10, first run): totals and startup-subtracted estimates

Run by the user on the same machine with
`bench\run-benchmarks.ps1 -ForgePath C:\Forge\esr153\forge.exe -Iterations 7`
(raw data: `bench/results/2026-11-10_145510.csv`). Forge is the Phase 7.95
build: the Phase 7.9 fixes, async stack capture off by default, on
SpiderMonkey ESR 153.8. The Bun and Node versions were not recorded. Node's
JSON time is far lower than in the README's earlier table (546 ms then,
338 ms now), which suggests Node was upgraded in between, so compare
across dates with care.

#### Total time (what the harness measures)

Median of 7 runs, wall-clock time for the whole process. Ratio < 1.0 means
Forge is faster.

| Benchmark | Forge (ms) | Bun (ms) | Node (ms) | Forge/Bun | Forge/Node |
|---|---:|---:|---:|---:|---:|
| `startup.js` | 17.47 | 48.39 | 48.22 | 0.36x | 0.36x |
| `json-bench.js` | 406.81 | 268.71 | 338.14 | 1.51x | 1.20x |
| `loop-bench.js` | 68.64 | 78.32 | 97.83 | 0.88x | 0.70x |
| `timer-bench.js` | 19.87 | 57.23 | 59.49 | 0.35x | 0.33x |
| `microtask-bench.js` | 42.10 | 61.89 | 110.11 | 0.68x | 0.38x |
| `promise-chain-bench.js` | 32.47 | 52.77 | 53.73 | 0.62x | 0.60x |
| `fs-bench.js` | 1,024.42 | 1,159.79 | 1,045.55 | 0.88x | 0.98x |
| `fs-async-bench.js` | 1,057.74 | 1,356.26 | 1,517.84 | 0.78x | 0.70x |

#### What the totals hide: startup decides the short benchmarks

**Do not quote the table above as "Forge is faster than Bun at
promises".** Every number in it includes process startup. Forge starts in
17 ms; Bun and Node need 48 ms. That 31 ms head start is larger than the
whole workload of four of these scripts, so it decides who "wins" them.

Subtracting each runtime's own `startup.js` median gives an estimate of the
time spent on the actual work:

| Benchmark | Forge (ms) | Bun (ms) | Node (ms) | Forge/Bun | Forge/Node | Work per step (Forge / Bun / Node) |
|---|---:|---:|---:|---:|---:|---|
| `json-bench.js` | 389.3 | 220.3 | 289.9 | 1.77x | 1.34x | 1.95 / 1.10 / 1.45 us per round trip |
| `loop-bench.js` | 51.2 | 29.9 | 49.6 | 1.71x | 1.03x | n/a |
| `timer-bench.js` | 2.4 | 8.8 | 11.3 | 0.27x | 0.21x | 0.48 / 1.77 / 2.25 us per timer |
| `microtask-bench.js` | 24.6 | 13.5 | 61.9 | 1.82x | 0.40x | 123 / 68 / 309 ns per callback |
| `promise-chain-bench.js` | 15.0 | 4.4 | 5.5 | 3.42x | 2.72x | 300 / 88 / 110 ns per step |
| `fs-bench.js` | 1,007.0 | 1,111.4 | 997.3 | 0.91x | 1.01x | n/a |
| `fs-async-bench.js` | 1,040.3 | 1,307.9 | 1,469.6 | 0.80x | 0.71x | n/a |

What this run supports:

- **Startup:** Forge is about 2.8x faster to start. Forge is also a far
  smaller runtime today (no module loader, no console, almost no web
  APIs), so expect this gap to shrink as features are added.
- **Async file I/O:** Forge is faster than both (0.80x Bun, 0.71x Node).
- **Sync file I/O:** a tie with Node (1%, inside the noise), ahead of Bun.
- **Timers:** Forge is ahead, but the workload is only a few milliseconds
  (5,000 timers), so treat this as indicative.
- **Loop (`fib(34)`):** level with Node; Bun is about 1.7x faster.
- **Microtasks:** Bun is about 1.8x faster; Forge is 2.5x faster than Node.
- **JSON:** Bun is about 1.8x faster, Node about 1.3x faster.
- **Promise chains:** still the biggest gap. Bun is about 3.4x faster and
  Node about 2.7x faster per step.

Limits of the subtraction: it is an estimate. `startup.js` measures
"start, print one line, exit", and a runtime may do extra lazy
initialisation when a script uses more features. For the rows where the
work is under 30 ms, a few milliseconds of noise changes the ratio a lot.

#### Compared with earlier runs

- **Promise chains improved about 4.6x in work time.** Total time went from
  88.84 ms (the README's earlier table, 142 engine) to 32.47 ms; with
  startup subtracted, roughly 69 ms to 15 ms. Two causes, measured
  separately as single runs on 2026-11-09: turning async stack capture off
  (96 ms to 63 ms on the 142 engine) and moving to ESR 153 (63 ms to 42 ms).
- **Sync file I/O lost ground and this is unexplained.** Forge/Bun went
  from 0.76x (2026-08-02) to 0.88x, and Forge/Node from 0.86x to 0.98x.
  All three runtimes were slower in absolute terms this time (Forge 803 ms
  to 1,024 ms, Bun 1,054 to 1,160, Node 938 to 1,046), so part of it is
  the machine or disk state, but Forge slowed by 28% against 10-11% for
  the others. Async file I/O moved much less (Forge +13%, Bun +9%, Node
  +7%). Candidates, none profiled: Phase 7.9's always-on bounds checks in
  the forge-core containers (file reads and path conversion append one
  element at a time), the new GC settings, the engine change, or plain
  I/O noise (Forge's seven runs ranged from 922 ms to above 1,024 ms).

#### Open items after this run

- **Done 2026-11-11:** the harness now records each script's own work time
  (`work-ms`, measured with `Date.now()` around the workload), prints the
  comparison on work time, and saves the machine and runtime versions. The
  smallest workloads were made larger (see "How the benchmarks work"). A
  run with the new harness will replace the estimated work times above.
- Check the sync file I/O shift: profile `File::ReadAllBytes` and the
  bounds checks added in Phase 7.9. (The 142-based build that could have
  isolated the engine change is retired.)
- The promise-chain gap (about 3.4x against Bun in work time) remains the
  main throughput target, followed by JSON and microtasks.

### Methodology

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

### Results (2026-07-30)

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

### Per-benchmark analysis

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

### Known open items

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

### Environment

- Forge: `C:\Forge\bin\forge.exe`, built from the Phase 7.3
  real-build-confirmed `forge.cpp` (see `HISTORY.md`).
- Bun: `C:\Users\varad\.bun\bin\bun.exe`.
- Node: `C:\nvm4w\nodejs\node.exe`.
- Harness: `bench/run-benchmarks.ps1`, default `-Iterations 5`.
- Date: 2026-07-30.

### Results (2026-08-02) — adds `fs-async-bench.js` (Phase 7.5.6)

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

