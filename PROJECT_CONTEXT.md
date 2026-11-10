# Forge Core

## Source of truth

**As of 2026-11-10 (Phase 7.95) this repository,
github.com/varadTheDeveloper/forge, is the source of truth.** The
SpiderMonkey engine lives outside it: a Firefox ESR checkout pinned in
`engine/engine.toml`, into which `tools\setup-engine.ps1` and
`tools\build.ps1` copy this repository as `js/src/forge`. See `CLAUDE.md`
→ "Build".

Retired, do not develop in or sync changes to them:
`C:\spidermonkey-dev\gecko-dev\js\src\forge\` (the only tracked copy
from 2026-07-29 until Phase 7.95, inside the old 142 Nightly tree) and
`C:\forge-beta\forge-runtime-beta\` (the original working copy).

## Vision

Forge Core is a modern C++ systems library being built as the foundation of
the Forge JavaScript Runtime. Written to compile as C++17. Since Phase
7.95 the real build (`mach build` on SpiderMonkey ESR 153) compiles
everything as C++20, because the engine's headers need it, but forge-core
keeps to the subset that is also valid C++17 and CI builds its tests both
ways; see `AGENTS.md` → "Coding Standards" and `HISTORY.md` → "Real build
environment discovered" for the history.

The goal is to build a production-quality runtime that can eventually
compete with Node.js, Deno, and Bun.

Forge Core is the low-level platform library that provides memory,
containers, filesystem, threading, synchronization, networking and platform
abstractions. The runtime itself (`forge/forge.cpp`, `forge/main.cpp`) embeds
SpiderMonkey and is expected to be rebuilt on top of Forge Core over time,
rather than using raw `std::` containers and the event-loop code currently
prototyped there.

Everything must be production quality.

---

# Philosophy

- Zero exceptions
- Explicit error handling
- Cross-platform
- RAII
- UTF-8 internally
- Small, clean API
- No unnecessary dependencies
- Performance first
- Readability over cleverness

---

# Development Rules

Every component follows this workflow.

1. Design API
2. Review API
3. Freeze API
4. Implement in small parts
5. Review implementation
6. Freeze implementation

Once frozen, APIs are NOT redesigned except for:

- Bugs
- Security issues
- Incorrect behaviour
- Fundamental design flaws

Never redesign frozen components. See `HISTORY.md` for what is already
frozen before touching any existing header. See `AGENTS.md` for how an AI
agent specifically should approach a task in this repo (review checklist,
"be honest about compiling", the include/naming pitfalls already hit
once).

---

# Error Handling

Constructors never fail.

Functions that may fail return `Result<T>` (or `ResultVoid` for functions
with no value to return).

Objects requiring allocation during creation use a static factory:

```
Create(...)
```

Example: `String::Create(...)` instead of `String(...)`.

`Error` is a small value type (`ErrorCode` + native platform code), storing
no message, no stack trace, no formatting. It never allocates and never
throws. See `forge/forge-core/Error.md` for the full frozen spec and
`forge/forge-core/Error.h` for the implementation.

`Failure` exists purely to disambiguate constructing a failed `Result<T>`
from a `Result<T>` that legitimately holds an `Error` as its value type.

---

# Memory

All allocations go through `memory::Allocator`.

Never use:

- `new`
- `delete`
- `malloc`
- `free`

Allocator contract:

- `Allocate(size, alignment)` returns `nullptr` on failure.
- `Deallocate(memory, size, alignment)` must receive the same size/alignment
  used during the matching `Allocate()` call.

`DefaultAllocator` is the current concrete allocator. `UniquePtr`/`MakeUnique`
and `Vector` are built on top of the `Allocator` interface, not on
`std::allocator`.

---

# Current Progress

Completed (API + implementation present in `forge/forge-core/`):

- Types (`Types.h` — fixed-width integer/float aliases, `Size`, `Offset`, `Byte`)
- Error (`Error.h`, spec in `Error.md`)
- Failure (`Failure.h`)
- Result / ResultVoid (`Result.h`, `Result.inl`, `ResultStorage.h`,
  `ResultVoid.h`, `ResultVoid.inl`, `ResultFwd.h`)
- Assert (`Assert.h`)
- Allocator / DefaultAllocator (`memory/Allocator.h`,
  `memory/DefaultAllocator.h/.cpp`, `memory/detail/AllocationBackend.h/.cpp`)
- UniquePtr / MakeUnique (`memory/UniquePtr.h/.inl`, `memory/MakeUnique.h/.inl`)
- Vector (`memory/Vector.h/.inl`)
- String / StringView / Span (`Span.h`, `StringView.h`, `String.h/.inl`)
- Array / Stack / Queue / Hash / HashMap / HashSet (`Array.h`, `Stack.h/.inl`,
  `Queue.h/.inl`, `Hash.h`, `HashMap.h/.inl`, `HashSet.h/.inl`)
- Path (`Path.h/.inl`, spec in `Path.md`) — fully verified, no OS dependency
- File (`File.h/.cpp`, spec in `File.md`) — **done, real-build-confirmed**.
  A real `mach build` on 2026-07-29 caught a genuine bug
  (`CreateDirectory` colliding with `<windows.h>`'s
  `CreateDirectory`→`CreateDirectoryA`/`W` macro), fixed by renaming the
  method to `MakeDirectory` — see `HISTORY.md`. A second real `mach
  build` afterward compiled clean, confirming the fix. Phase 3 is
  complete.
- Sync — Mutex / ConditionVariable / LockGuard (`Mutex.h/.cpp`,
  `ConditionVariable.h/.cpp`, `LockGuard.h`, spec in `Sync.md`) — **done,
  fully real-build-confirmed** (2026-07-30). `LockGuard` was always fully
  verified (pure logic, no OS dependency). `Mutex`/`ConditionVariable`
  are now confirmed three ways: a real `python mach build` (Gecko's own
  toolchain), a standalone MSVC `cl` compile against the real
  `<windows.h>`, and `ThreadingSmokeTest.cpp` passing against real OS
  threads (mutex stress test, condition-variable handoff and timeout).
  See `HISTORY.md`'s "Phase 4 fully real-build-confirmed" entry.
- Thread / ThreadPool / ErasedCallable (`Thread.h/.inl/.cpp`,
  `ThreadPool.h/.inl/.cpp`, `ErasedCallable.h`, spec in `Thread.md`) —
  **done, fully real-build-confirmed** (2026-07-30). `ErasedCallable` was
  always fully verified (pure logic, no OS dependency). `Thread`/
  `ThreadPool` are now confirmed the same three ways as `Mutex`/
  `ConditionVariable` above, including `ThreadingSmokeTest.cpp`'s
  4-worker/500-task run (every task ran exactly once, `Submit` after
  `Shutdown()` correctly failed). A real MSVC-specific bug
  (`ThreadingSmokeTest.cpp`'s `kIterations` lambda-capture, C3493 — GCC/
  Clang exempt read-only `constexpr` locals from capture, MSVC doesn't)
  was caught and fixed along the way. Phase 4 is complete.

All of the above compiled cleanly and passed a runtime test (including
allocator-failure paths) as of 2026-07-26 — see `HISTORY.md` for the list
of bugs that pass fixed. Before that date this had never actually been
compiled end-to-end. String/StringView/Span were added and verified
2026-07-27, Array/Stack/Queue/Hash/HashMap/HashSet 2026-07-29, Path/File
2026-07-29 (File real-build-confirmed the same day after a rename fix),
and Sync/Thread/ThreadPool 2026-07-29, all under the corrected
C++17/no-exceptions constraints (see `AGENTS.md`) rather than the
earlier, wrong C++20 assumption.

`forge/platform/` (Windows path handling groundwork) existed earlier in
the project but was deliberately removed; the filesystem layer (Phase 3)
was designed from scratch rather than building on it, starting from two
frozen spec docs (`Path.md`, `File.md`) per this phase's own requirement.
See `HISTORY.md`.

- Runtime integration (`forge.cpp`) — **done, real-build-confirmed and
  benchmarked**, corrected 2026-07-30. `forge.cpp`'s microtask queue and
  timer registry now use `Queue<memory::UniquePtr<Microtask>>` and
  `HashMap<int, memory::UniquePtr<JsTimer>>` instead of
  `std::vector`/`std::unique_ptr`, script loading uses `Path`/`File`
  instead of `std::ifstream`/`std::stringstream`, and
  `EnqueueMicrotask`/`SetTimeout`/`SetInterval` route allocation through
  `memory::MakeUnique<T>` instead of `std::make_unique`. Sandbox
  verification compiled every container/allocator call used against the
  real `forge-core` headers (not mocks) under g++/clang++ with full
  warnings, clean under ASan+UBSan and valgrind. The user then built this
  exact `forge.cpp` with a real `mach build` and ran the full
  `bench/run-benchmarks.ps1` suite against it (2026-07-30) — see
  `ROADMAP.md`'s Phase 6 entry and `Forge_Benchmark_Report.md` for the
  full same-machine Forge/Bun/Node results across all six benchmark
  scripts, and for the one still-open item (a `promise-chain-bench.js`
  performance gap not yet explained by profiling — kept separate from
  this "done" status since it's follow-up work, not a blocker). See
  `HISTORY.md`'s "Phase 6 corrected"/"Phase 6 benchmarked" entries: an
  earlier session had reported this same work as delivered and confirmed
  by a real `mach build`, but the actual `forge.cpp` on disk never
  contained these changes at the time — that report did not reflect
  reality; this entry reflects what has now actually been verified.

- Filesystem JS API, Phase 7.2 (JS/native marshalling primitives in
  `forge.cpp`) — **fully real-build-confirmed** (2026-07-30). The six
  helpers themselves, a follow-up `forge --self-test` smoke test suite
  (12 cases, exercising all six against a live `JSContext`/`Realm`), and
  a linkage cleanup have all now been through a real `python mach build`,
  with `forge --self-test` printing `[self-test] ALL PASSED` (exit code
  0). See `ROADMAP.md`'s Phase 7.2 entry and `HISTORY.md` for the full
  verification account, the two binary-marshalling design decisions
  made, and the `JsBindings.md` behavioral clarification recorded
  alongside it. Phase 7.1 (`JsBindings.md`/`Fs.md`) remains frozen and
  unchanged.

- Filesystem JS API, Phase 7.3 (synchronous `fs.*Sync` bindings) —
  **fully real-build-confirmed** (2026-07-30). All seven methods from
  `Fs.md`'s Public API (`readFileSync` through `statSync`) are wired to
  `globalThis.fs` in `forge.cpp`, and the 17-case addition to
  `forge --self-test` (`RunFsSmokeTests`) exercising every method's
  success and failure path has now been through a real `python mach
  build`, with `forge --self-test` printing `[self-test] ALL PASSED`
  across all 29 cases (the original 12 plus these 17), exit code 0. See
  `ROADMAP.md`'s Phase 7.3 entry and `HISTORY.md` for the full
  verification account (including the sandbox-only compile check and
  direct-native-function invocation work done before this real build, for
  the record).

- Filesystem JS API, Phase 7.4 (benchmarks) — **done** (2026-07-30).
  `bench/fs-bench.js` written and run through the updated
  `run-benchmarks.ps1` against real `forge.exe`/`bun.exe`/`node.exe` on
  the user's machine: Forge/Bun ratio 0.86x, Forge/Node ratio 0.91x
  (Forge faster than both). This run also confirmed, for the first time,
  that the full seven-script bench suite runs clean end-to-end on all
  three runtimes with no failures — including the timer/microtask/
  promise-chain batch that had never been run end-to-end before. See
  `ROADMAP.md`'s Phase 7.4 entry and `HISTORY.md` for the full table.
  Along the way, this file and `ROADMAP.md`/`HISTORY.md`'s Phase 6
  entries were found to reference a `Forge_Benchmark_Report.md` that did
  not actually exist anywhere in the repo (confirmed via a direct
  directory listing) — raised with the user directly rather than
  silently resolved either way; the user asked for a fresh report
  authored from today's real numbers. `Forge_Benchmark_Report.md` now
  exists, dated 2026-07-30, with all seven scripts' results and its own
  Provenance Note documenting the discrepancy rather than presenting
  itself as a recovered original. **Phase 7, all of it (7.1-7.4), is now
  done.**

- Networking, Phase 5 (`Socket`/`IpAddress`) — **done, fully
  real-build-confirmed** (2026-07-30). `IpAddress` was always pure-logic/
  no-OS-dependency and fully sandbox-verified. `Socket` needed real
  wiring work this round: `Socket.cpp` had never been added to
  `moz.build`'s `SOURCES` (added, correctly case-insensitive sorted), and
  no `OS_LIBS` entry for `ws2_32` existed either despite `Socket.cpp`
  assuming one would be (added). `SocketSmokeTest.cpp` (never previously
  run) had two real bugs fixed along the way: the same MSVC-specific
  lambda-capture issue found in `ThreadingSmokeTest.cpp`, and an
  incorrect build-command reference to compiling `IpAddress.inl`
  directly. All three legs of verification are now real: `python mach
  build` succeeded (`Socket.cpp` compiles and links against `ws2_32`
  through Gecko's own toolchain), a standalone MSVC `cl` compile
  succeeded against the real `<winsock2.h>`/`<ws2tcpip.h>`, and
  `SocketSmokeTest.cpp` passed against a real loopback TCP connection
  (echo round-trip byte-verified, plus a real connection-refused failure
  path). An earlier session had reported this phase as real-build-
  confirmed without that reflecting what was actually on disk (discovered
  2026-07-30 alongside the Runtime Integration correction above — see
  `HISTORY.md`); this entry is the real thing. With Phase 5 done, every
  phase in this project (1 through 7) now has real-build evidence behind
  it — no outstanding real-build debt remains.

- Filesystem JS API, Phase 7.5 (async `fs`) — **complete end to end,
  7.5.1-7.5.6 all done and real-build-confirmed** (2026-08-02, see
  below). Chosen over `net.*` bindings, worker-thread
  bindings, and a minimal module system as the highest-impact next
  milestone that builds on already-verified foundations (`ThreadPool`)
  rather than opening a large new design problem. `FsAsync.md` (delivered,
  reviewed, frozen) specifies Promise-returning
  `fs.readFile`/`writeFile`/`appendFile`/`exists`/`mkdir`/`rm`/`stat`,
  dispatched onto `ThreadPool`. Grounded in direct reads of `IocpLoop.h`/
  `ThreadPool.h`/`forge.cpp`'s `Runtime` class, it surfaced three
  previously-unflagged risks, all now closed: `Runtime::run()`'s
  event-loop exit condition didn't account for in-flight async fs
  operations (fixed via a `pendingAsyncOps_` counter on `Runtime`, not
  `IocpLoop` — 7.5.2, real-build-confirmed against a genuine async
  operation, not just proven inert); the cross-thread `PostCompletion`
  wakeup pattern this phase depends on had never been tested on a real
  machine (7.5.1, `IocpCrossThreadSmokeTest.cpp`, real-machine-confirmed
  — a worker thread's `PostCompletion` call genuinely woke the main
  thread out of `RunOnce()`'s wait); and native `JS::Promise`
  construction/resolution was entirely new binding surface (its exact
  signatures fetched from the real `js/public/Promise.h` on
  `mozilla/gecko-dev`, not guessed, mirroring Phase 7.2's
  `JS_NewPlainObject` precedent). All seven `fs.*` async methods are now
  implemented and real-build-confirmed: `fs.readFile` first (7.5.3, its
  own `AsyncFsReadOp` struct), then the remaining six
  (`writeFile`/`appendFile`/`mkdir`/`rm`/`exists`/`stat`, 7.5.4, sharing
  one `AsyncFsMiscOp` struct discriminated by a `kind` enum rather than
  repeating the pattern six more times). `manual-test-fs-async.js` (an
  interim, non-`--self-test` verification script) exercises all seven
  methods' success and failure paths and passed cleanly on first real
  build for both 7.5.3 and 7.5.4. 7.5.5 (the formal Promise-based
  self-test harness) is now done and real-build-confirmed: a new
  `RunFsAsyncSmokeTests(cx, runtime)` in `forge.cpp`, run from
  `--self-test` — the first suite in this file that actually calls
  `Runtime::run()`, since every prior suite (Phase 7.2/7.3) calls a
  binding function directly and inspects a `Result<T>` synchronously. It
  evaluates the same seven-method chained sequence
  `manual-test-fs-async.js` already exercises (12 cases, recorded via a
  native `globalThis.__asyncSelfTestRecord` callback since native code
  can't itself await a `Promise`), plus a second, separate script whose
  only statement is one bare `fs.readFile(...).then(...)` call with no
  timers pending — the direct, isolated regression test for the Phase
  7.5.2 `pendingAsyncOps_` fix FsAsync.md's Acceptance Criteria
  specifically calls for. All 13 expected `[self-test]` lines passed on
  the real machine (`python mach build` + `forge --self-test`), including
  calling `Runtime::run()` a second time on the same `Runtime` instance
  (once per test group) working correctly — a new usage pattern for
  `run()`, now real-build-confirmed rather than just reasoned through.
  `manual-test-fs-async.js` is superseded by this suite as the feature's
  ongoing regression check going forward. 7.5.6 (benchmarks + docs) is
  **done**: `bench/fs-async-bench.js` (mirrors `fs-bench.js`'s iteration
  counts/payload, chained through `.then()` rather than a blocking loop,
  per FsAsync.md's Non-Goals) initially failed against Forge specifically
  with `fs.mkdir is not a function` on the first benchmark run — a real,
  surprising discrepancy given `forge --self-test` had just shown
  `fs.mkdir` passing twice moments earlier — but this turned out to be an
  environment issue, not a code bug: `C:\Forge\bin\forge.exe` (the binary
  `run-benchmarks.ps1` runs) hadn't been refreshed from `mach build`'s
  real output since before Phase 7.5's async work landed. The user copied
  the fresh binary over and reran; the corrected run produced real Forge
  numbers (`fs-async-bench.js`: forge median 935.39 ms, forge/bun 0.75x,
  forge/node 0.66x — Forge faster than both, consistent with
  `fs-bench.js`'s own sync-path result), and the other seven scripts
  stayed within a few percent of the 2026-07-30 baseline.
  `Forge_Benchmark_Report.md` and `HISTORY.md` have the full writeup of
  both the discrepancy and its resolution. See `ROADMAP.md`'s Phase 7.5
  entry for the full milestone breakdown. **No known open items remain
  for Phase 7.5.**

See `ROADMAP.md` for the intended order of the remaining work, and
`HISTORY.md` for decisions already frozen on the completed components.

---

# Coding Style

- Header-only implementations use `.inl`
- Public API in `.h`
- Inline function bodies in `.inl`
- `constexpr` whenever possible
- `noexcept` whenever possible
- `[[nodiscard]]` where appropriate
- Namespace: `forge::core` for core library types
- Includes: relative, directory-correct paths (`"Types.h"`, `"../Error.h"`,
  `"memory/ResultFwd.h"`) — never a `forge/core/...`-style rooted path.
  The physical directory is `forge/forge-core/` (hyphenated), and relative
  includes work regardless of how the surrounding build's include roots
  are configured. See `HISTORY.md` for why this matters (most of the
  codebase used the wrong convention until the 2026-07-26 pass).
- If a member function's name matches a type visible in the enclosing
  namespace (e.g. `Error()` returning `Error&`, `Size()` returning `Size`),
  GCC hard-errors (`-Wchanges-meaning`) rather than just warning. Fix with
  the `class Error` elaborated-type-specifier for real class types, or by
  fully qualifying the alias target (`forge::core::Size`) when the type is
  a `using`-alias. Do not rename the accessor to work around this — see
  `AGENTS.md` for the full explanation.

Component specs (like `Error.md`) follow this shape: Purpose, Responsibilities,
Non-Goals, Design Goals, Public API, Memory Layout, Ownership, Error Handling
Policy, Thread Safety, Dependencies, Extensibility, Acceptance Criteria,
Implementation Status. Write a matching spec doc before freezing any new
component's API.

---

# Long-Term Goal

Forge Core is not intended to be merely another container library.

It is the complete systems foundation for the Forge JavaScript Runtime.

Every design decision should consider:

- Performance
- Maintainability
- Explicit APIs
- Consistency
- Future runtime requirements
