# Forge Roadmap

Goal: Forge is being built to be a genuine, credible alternative to Bun —
not just "a JS runtime that also runs," but one that is fast enough to be
chosen over Bun for real workloads. This roadmap is ordered so that the
decisions with the biggest effect on speed happen early, before more
features get built on top of an event loop or allocator strategy that
would need to be ripped out later.

Do not start a later phase's component ahead of an earlier one unless a
specific need pulls it forward — note the reason in `HISTORY.md` if that
happens.

Completed components (Types, Error, Failure, Result/ResultVoid, Assert,
Allocator/DefaultAllocator, UniquePtr/MakeUnique, Vector) are out of scope
here — see `PROJECT_CONTEXT.md` → Current Progress and `HISTORY.md` for
their status. That existing foundation is sound and is not being
rearchitected — see the note at the bottom of this file.

---

## Phase 0 — Benchmark Harness & Baseline

Before building anything else, set up a repeatable way to measure Forge
against Bun (and Node, for a second reference point) on the same machine.
Without this, "faster than Bun" is a feeling, not a fact, and every later
architecture decision in this roadmap is unverifiable without it.

Concrete benchmarks to track from day one:

- Cold start time (`forge script.js` vs `bun script.js`, empty script).
- Raw JS execution (a fixed microbenchmark — e.g. a Fibonacci/loop suite).
  This one is heavily engine-bound (SpiderMonkey vs JavaScriptCore) and
  Forge may simply not win it — track it anyway so it's a known, not a
  surprise.
- JSON parse + stringify throughput on a realistic payload size.
- Filesystem read/write throughput (once Phase 5 lands).
- HTTP requests/second on a minimal echo server (once Phase 6 lands).

Keep results in a simple table/log in the repo, updated as each phase
lands, so progress is visible and regressions are caught immediately.

## Phase 1 — String / StringView / Span

Almost everything downstream touches strings (source text, property names,
JSON, paths, HTTP headers), so this has to be solid before the native
fast-path work in Phase 5/6. Same rules as everything else: constructors
never allocate, allocation-requiring construction goes through
`String::Create(...)`, UTF-8 internally.

## Phase 2 — Event Loop Rearchitecture (the single highest-leverage decision)

`forge.cpp`'s current `EventLoop::run()` busy-polls in a `while` loop with
a 1ms `sleep`, checking `TimerQueue`/`ForgeJobQueue` on every wake-up. This
is fine for a prototype and will never be competitive — it wastes CPU and
adds up to 1ms of latency to every event regardless of load.

Replace it with a loop built on **IOCP (I/O Completion Ports)** — the
Windows equivalent of what Bun gets from avoiding libuv and talking close
to the OS. Concretely:

- Design a small OS-abstraction interface now (e.g. `forge::platform::IoLoop`)
  even though only the Windows/IOCP backend is implemented first, so a
  Linux backend (io_uring or epoll) can be added later without touching
  callers.
- Timers and microtasks get driven by completion notifications instead of
  a manual sleep-and-poll cycle.
- Async file I/O, socket accept/read/write all get wired through the same
  completion port, not separate ad-hoc mechanisms.

This is the one decision in this whole roadmap most worth getting right
before building more on top, because retrofitting it later means touching
every API built on the old loop.

**Progress (2026-07-27):** `forge::core::platform::TimerScheduler`
(pure timer bookkeeping, no OS calls), `IoLoop` (the compile-time backend
alias), and `IocpLoop` (the Windows/IOCP backend itself, wrapping
`TimerScheduler`) are written and **confirmed building and passing on the
project owner's real Windows machine** (`IocpSmokeTest.cpp` — real
`CreateIoCompletionPort`/`GetQueuedCompletionStatus`/
`PostQueuedCompletionStatus`, a real repeating timer, `RequestStop()`, all
verified working). `forge.cpp`'s old `EventLoop`/`TimerQueue` busy-poll
loop has now been replaced with `IoLoop` — `setTimeout`/`setInterval`/
`clearTimeout` schedule through it, `Runtime::run()` blocks for the next
timer instead of sleeping-and-polling. Two real bugs (a GC-tracing gap
that pre-dated this change, and a use-after-teardown lifetime bug
introduced by giving `run()` an early-exit path) were caught and fixed
during this pass — see `HISTORY.md` for both. **This forge.cpp change has
not been built anywhere yet** — SpiderMonkey's API surface is too large to
mock faithfully in the sandbox this was written in (unlike `IocpLoop`,
where the ~10-function Win32 surface could be mocked precisely), so this
one genuinely needs a real Visual Studio build before it's trusted, the
same way the earlier `VectorSmokeTest.cpp` MSVC error was only found once
built for real.

## Phase 3 — Allocator Hardening for Hot Paths

The `Allocator` interface already in forge-core is exactly the right
foundation for this — it's already pluggable, nothing built on it needs to
change. What's missing is a second, faster implementation for hot,
short-lived allocations: an arena/bump allocator for per-request or
per-tick native buffers (parsing, temporary formatting, request handling),
swapped in wherever `Vector`/`String`/etc. accept a custom `Allocator&`.

Keep this cleanly separate from SpiderMonkey's own GC, which manages actual
JS-heap objects — this allocator work is only for the native C++ side of
the runtime, not JS values.

## Phase 4 — Collections

- Array
- HashMap
- HashSet
- Queue
- Stack

Needed internally by the runtime itself (module registry, HTTP header
maps, event listener tables) as much as by user-facing APIs.

## Phase 5 — Filesystem

The earlier `forge/platform/` groundwork was deliberately removed (see
`HISTORY.md`) — this starts from a clean slate, and now explicitly built
on the Phase 2 IOCP event loop from the start (async by construction, not
retrofitted). Freeze a path/filesystem spec doc (matching the shape of
`Error.md`) before building the public API.

## Phase 6 — Networking & a Minimal HTTP Server

Raw TCP/UDP sockets on top of the Phase 2 event loop, then a minimal,
natively-implemented HTTP server (a `Forge.serve()`-equivalent). This is
the flagship benchmark most people actually compare Bun against — it
deserves a hand-written, tightly-optimized native implementation talking
directly to SpiderMonkey's C API, not a JS-level wrapper over generic
primitives (that's precisely why Bun's version is fast).

## Phase 7 — Threading

Worker/thread-pool primitives (mutex, condition variable, thread pool),
built on `memory::Allocator` and returning `Result`/`ResultVoid`. Used to
keep genuinely blocking work (some fs operations, DNS, crypto) off the
main JS thread without complicating the single-threaded event-loop model
above — this is what Node/Bun/Deno all do, not a compromise.

## Phase 8 — Runtime Integration

Replace the remaining prototype pieces still living directly in
`forge/forge.cpp` (`std::vector<std::unique_ptr<Microtask>>`, the original
`TimerQueue`/`ForgeJobQueue` if anything of them survives Phase 2) with
Forge Core equivalents (`Vector`, `UniquePtr`, the Phase 4 collections),
and route SpiderMonkey error/failure paths through `Result<T>` where
practical instead of raw `bool`/`printf` returns.

## Phase 9 — Startup Time (Bytecode Caching / Snapshotting)

Bun's near-instant cold start is a real, noticeable difference for CLI/
scripting use cases. Investigate caching SpiderMonkey's self-hosted code
and precompiling frequently used built-ins so `forge script.js` doesn't
pay full engine-init cost on every invocation. Measure against the Phase 0
cold-start benchmark.

## Phase 10 — Module System & Package Compatibility

CommonJS + ESM module resolution, `package.json` handling, and enough
npm-style compatibility that real packages can run. This is about
adoption/usability rather than raw speed, which is why it's last — a
runtime nobody can actually use to run real code doesn't get to the speed
comparison at all, but it's not where the "beat Bun" battle is won or
lost, so it doesn't need to block the performance work above it.

---

## Why the existing foundation isn't being redone

SpiderMonkey as the engine, and the zero-exception/`Result<T>`/pluggable-
`Allocator` design in forge-core, are both being kept as-is going forward.
Neither needs to change for any of the phases above — Phase 2's event loop
and Phase 3's arena allocator both build *on top of* the existing
`Allocator` interface rather than replacing it. The place where Forge can
realistically compete with or beat Bun is in the I/O model, the allocation
strategy, and how deep the native (non-JS-shim) implementations go — not
in re-fighting the engine choice already made.
