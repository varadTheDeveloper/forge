# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

This repository (github.com/varadTheDeveloper/forge) is the source of truth
for Forge. The SpiderMonkey engine is **not** in it: the engine is a separate
Firefox ESR checkout, pinned in `engine/engine.toml` (default location
`C:\forge-engine\esr153`), and the build copies this repository into
`js/src/forge` inside that checkout.

Retired, never edit or sync: `C:\spidermonkey-dev\gecko-dev\js\src\forge`
(the old 142 Nightly tree, frozen since Phase 7.95) and
`C:\forge-beta\forge-runtime-beta\`.

Two layers live here:

- **`forge-core/`** — a from-scratch C++17 systems library (memory, containers,
  string/path, filesystem, sync, threads, sockets, Windows IOCP event loop) in
  namespace `forge::core`. It deliberately avoids the STL and the CRT allocator.
- **`forge.cpp`** — the Forge JavaScript runtime: a single large translation unit
  that embeds SpiderMonkey and is being rebuilt on top of forge-core over time.

## Required reading before changing code

`AGENTS.md` (how to work here), `PROJECT_CONTEXT.md` (vision + what is frozen),
`ROADMAP.md` (phase order), `HISTORY.md` (frozen decisions and the bugs that
produced the rules below). Component specs — `Fs.md`, `FsAsync.md`,
`JsBindings.md`, `forge-core/*.md` — are frozen API contracts; write a matching
spec before freezing any new component.

Work is organized as numbered Phases (1-7 done). Don't start a later phase's
component ahead of an earlier one without recording the reason in `HISTORY.md`.

## Build

One-time setup (needs MozillaBuild at `C:\mozilla-build`, and Python 3.12 or
older on PATH as `python`; Python 3.14 breaks mach):

```
.\tools\setup-engine.ps1          # fetch the pinned engine, add Forge to it
cd C:\forge-engine\esr153
python mach bootstrap              # choose "SpiderMonkey JavaScript engine"
```

Every build after that, from this repository:

```
.\tools\build.ps1 -Test           # or: build.bat -Test
```

`tools\build.ps1` copies this checkout into the engine tree, runs `mach build`,
installs `forge.exe` **and `mozglue.dll`** (forge.exe imports it) to
`C:\Forge\bin`, and with `-Test` runs `forge --self-test` and the runtime
tests. Because it always installs after building, a stale
`C:\Forge\bin\forge.exe` (which once caused a false "missing binding" failure
during a benchmark run) can't happen as long as you build through it. CI runs
the same two scripts (`.github/workflows/ci.yml`, job `forge-windows-engine`).

`setup-engine.ps1` makes the only two changes Forge needs inside the engine
tree: it adds `"forge"` to `js/src/moz.build` and adds `js/src/forge/` to the
ignore list of `config/check_spidermonkey_style.py`.

`moz.build` lists the production `SOURCES`. A new `.cpp` under `forge-core/`
must be added there (case-insensitively sorted) or it silently never compiles;
new OS dependencies go in `OS_LIBS` (a `#pragma comment(lib, ...)` alone is not
enough for the mach build).

## Tests

**Runtime self-test** — the primary regression suite, 30+ cases covering JS/native
marshalling, `fs.*Sync`, and Promise-returning async `fs.*`:

```
forge --self-test        # prints "[self-test] ALL PASSED", exit 0
```

It lives in `forge.cpp` (`RunMarshallingSmokeTests`, `RunFsSmokeTests`,
`RunFsAsyncSmokeTests`). The async suite is the only one that drives
`Runtime::run()`; the others call binding functions directly and inspect a
`Result<T>`. There is no per-case filter, so to run a single suite temporarily
edit the dispatch in `main()`.

**forge-core tests** — `*Test.cpp` / `*SmokeTest.cpp` files are standalone
harnesses using a local `CHECK` macro, **not** part of `moz.build`. Each file's
header comment carries its exact build command. They compile with MSVC directly,
e.g. from `forge-core\`:

```
cl /std:c++17 /EHs- /W4 ThreadingSmokeTest.cpp Mutex.cpp ConditionVariable.cpp Thread.cpp ThreadPool.cpp platform\Win32Error.cpp memory\DefaultAllocator.cpp memory\detail\AllocationBackend.cpp
```

Never pass a `.inl` to the compiler as its own translation unit; `.inl` files are
included by their `.h`.

`CMakeLists.txt` builds only `VectorSmokeTest.cpp` and is incidental.

**Benchmarks** — `bench\run-benchmarks.ps1` times every `bench\*.js` against
whichever of `forge.exe` / `bun.exe` / `node.exe` it finds, writes a CSV to
`bench\results\`, and prints forge/bun and forge/node ratios.

```
.\bench\run-benchmarks.ps1 -ForgePath "C:\Forge\bin\forge.exe" -Iterations 7
```

Results are written up in `Forge_Benchmark_Report.md`.

## Hard build constraints (each caused real bugs; see HISTORY.md)

- **The real build is C++20; forge-core stays C++17-compatible.** The ESR 153
  mach build compiles every file with `-std:c++20`, because the engine's
  headers require it, so `forge.cpp` is C++20. `forge-core/` includes no
  engine headers and keeps to the subset that also compiles as C++17 (the
  Manifesto and decision D5 say C++17), and `tools/ci/forge-core-tests.sh`
  builds every forge-core test both ways. So in `forge-core/`: no
  `std::construct_at` (use `forge::core::detail::ConstructAt` from `Construct.h`),
  no `<bit>`, no constexpr destructors, no concepts, `<=>`, `consteval`/`constinit`,
  no `= default` on a friend comparison operator.
- **Exceptions are disabled.** Unconditional `try`/`catch`/`throw` anywhere is a
  hard compile error. Guard with `#if defined(__cpp_exceptions)` when the code
  must also work where a caller-supplied type can throw (see `MakeUnique.inl`).
- **Sandbox compiles prove nothing.** Only a real `python mach build` (or a real
  MSVC build for out-of-tree smoke tests) justifies calling something confirmed.
  Say explicitly when verification was sandbox-only.
- **MSVC captures `constexpr` locals.** A lambda using a `constexpr` local must
  capture it (C3493); GCC/Clang do not require this, so sandbox-clean test code
  can still fail on the real machine.

## forge-core conventions

- Public API in `.h`, inline bodies in `.inl`, namespace `forge::core`
  (`forge::core::memory`, `forge::core::platform` for subsystems).
- **Includes are relative and directory-correct** (`"Types.h"`, `"../Error.h"`,
  `"memory/ResultFwd.h"`). Never a rooted `forge/core/...` path — the physical
  directory is `forge-core/`, hyphenated.
- **Constructors never fail.** Anything needing allocation uses a static
  `Create(...)` factory returning `Result<T>`; fallible functions return
  `Result<T>` or `ResultVoid`.
- `Result<T>`'s constructor from `Failure` is `explicit`: write
  `return Result<X>(Failure{...});`, not `return Failure{...};`.
- **No `new`/`delete`/`malloc`/`free`.** Allocate through `memory::Allocator`
  (`DefaultAllocator`), `MakeUnique`, `Vector`. `Deallocate` must be handed the
  same size/alignment as the matching `Allocate`.
- `Error` is a value type (`ErrorCode` + native platform code) that never
  allocates, throws, or carries a message.
- **Method names colliding with an enclosing-scope type** hard-error under GCC
  (`-Wchanges-meaning`). Fix with an elaborated specifier (`class Error& Error()`)
  for real class types, or by qualifying the alias target
  (`using SizeType = forge::core::Size;`) for `using`-aliases. Do not rename the
  accessor — `Error()`/`Size()` are frozen names.
- **Watch for `<windows.h>` macros.** `CreateDirectory` expands to
  `CreateDirectoryA`/`W`, which is why `File::MakeDirectory` is named that way.
- Prefer `constexpr`, `noexcept`, `[[nodiscard]]` wherever C++17 allows.

## Runtime architecture (`forge.cpp`)

`Runtime` owns everything: a `ForgeJobQueue` (a `JS::JobQueue` subclass that
drains the microtask queue; since Phase 7.95 the queue itself belongs to the
engine, see `js/friend/MicroTask.h`), a `JsTimerRegistry` (`HashMap<int, UniquePtr<JsTimer>>`
behind `setTimeout`/`setInterval`/`clearTimeout`), an `IoLoop`, and a shared
`ThreadPool` for async work. A pointer to it is stashed in a global reserved slot
and recovered via `GetRuntime(cx)` from binding functions.

`Runtime::run()` is the event loop: drain jobs, exit when the job queue, the
I/O loop, **and** `pendingAsyncOps_` are all empty, otherwise `loop_.RunOnce()`.
Any async operation must call `OnAsyncOperationStarted()` at dispatch and exactly
one matching `OnAsyncOperationFinished()` after its Promise settles, or the loop
either exits early or hangs.

Async `fs.*` follows one pattern: allocate an op struct (`AsyncFsReadOp`, or
`AsyncFsMiscOp` discriminated by a `kind` enum for the other six methods),
register it on `Runtime` so it is GC-traced and outlives any script reference to
its Promise, submit the blocking work to `WorkerPool()`, and have the worker call
`IoLoop::PostCompletion` to wake the main thread, which settles the Promise.

`IoLoop` is a compile-time alias (`platform/IoLoop.h`), currently only the
Windows `IocpLoop`; other platforms `#error` out. Ordering in `main()` matters:
the `JS::RootedObject` global lives in an inner scope so it is destroyed before
`JS_DestroyContext`.

## Honesty

Never claim code compiles or works without having actually run the build or the
test. If information is missing, ask. Do not invent APIs — fetch real JSAPI
signatures from the actual SpiderMonkey headers in this tree.
