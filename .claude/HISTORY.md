# Decision History

This file records decisions already made on Forge Core components. Read this
before touching any file under `forge/forge-core/` — if a component is
listed as Frozen here, do not redesign it; only bugs, security issues,
incorrect behaviour, or fundamental design flaws justify a change (see
`PROJECT_CONTEXT.md` → Development Rules).

---

## Types

Decision

Fixed-width aliases only (`i8`..`i64`, `u8`..`u64`, `f32`, `f64`, `Byte`,
`Size`, `Offset`). No higher-level type utilities live here.

Reason

Keep the lowest layer dependency-free and trivial; every other component
depends on this one, so it must never grow.

Status

Frozen.

---

## Error

Decision

`Error` stores only an `ErrorCode` (canonical, generic — no module-specific
codes like `FileNotFound`) and a `NativeError` (`i32`) platform code. No
message, no formatting, no stack trace, no logging, no exceptions.
`constexpr`/`noexcept` throughout, trivially movable, no heap allocation.

Reason

`Error` is embedded inline inside `Result<T>`, so it must stay small,
allocation-free, and cheap to copy/move. Formatting and logging are
explicitly out of scope so the type can stay platform-independent.

Status

Frozen. Full spec in `forge/forge-core/Error.md`. Future additions may only
add generic `ErrorCode` values or platform-specific conversion helpers
implemented outside the type — never allocation, logging, formatting, or
module-specific codes.

---

## Failure

Decision

`Failure` is a thin wrapper around `Error`, used only to disambiguate
constructing a failed `Result<T>` from a `Result<T>` whose value type
happens to be `Error` itself.

Status

Frozen.

---

## Result / ResultVoid

Decision

`Result<T>` is `[[nodiscard]]`, embeds `Error` directly (no dynamic
allocation for the error path), and exposes `HasValue()`, `Value()`,
`Error()`, `operator*`, `operator->`, `operator bool`, and `Ignore()` (to
explicitly discard a `[[nodiscard]]` result without warnings). Constructed
explicitly from `T`/`Failure`, never implicitly. `ResultVoid` is the
equivalent for functions that only need to report success/failure with no
value.

Reason

Forge Core uses no exceptions, so every fallible function must communicate
success/failure explicitly through its return type instead.

Status

Frozen.

---

## Assert

Decision

`FORGE_ASSERT(condition)` wraps `assert()` and is compiled out under
`NDEBUG`. It exists to catch programmer errors during development, not to
report recoverable runtime failures — that's what `Result<T>` is for.

Status

Frozen.

---

## Memory / Allocator

Decision

All allocation goes through the `memory::Allocator` interface
(`Allocate(size, alignment)` / `Deallocate(memory, size, alignment)`).
`Allocate` returns `nullptr` on failure rather than throwing.
`Deallocate` must be called with the exact size/alignment used at
allocation time. `DefaultAllocator` is the current concrete implementation,
backed by `memory::detail::AllocationBackend`.

Reason

Centralizing allocation behind one interface is what makes it possible to
later swap in arena/pool allocators, track memory, or sandbox allocation
for the JS runtime without touching call sites.

Status

Frozen (interface). `new`/`delete`/`malloc`/`free` must never be used
directly anywhere in Forge Core or code built on top of it.

---

## UniquePtr / MakeUnique

Decision

`UniquePtr` is an owning smart pointer built on top of `memory::Allocator`
(not `std::allocator`/`new`), constructed via `MakeUnique<T>(...)`.

Status

Frozen.

---

## Vector

Decision

API frozen. Implementation frozen. Built on `memory::Allocator`, not
`std::allocator`. Do not redesign.

Status

Frozen.

---

## Platform path handling (`forge/platform/`)

Decision

The earlier `forge/platform/` groundwork (`FilePath.cpp`,
`PathNormalizer.cpp`, `SeparatorNormalizer.cpp`, Windows-specific
`PathTraits.cpp`/`PathUtils.cpp`/`Platform.cpp`, and a parallel
`include/forge/...` header tree) has been removed. Phase 3 (Filesystem)
will design the path/filesystem layer from scratch rather than resuming
this code.

Reason

Deliberate decision by the project owner (2026-07-26), not an accident —
confirmed after the removal was flagged during a repo review (it showed up
as an uncommitted deletion against commit `326d50e`).

Status

Removed. Do not resurrect this code from git history without checking with
the project owner first; do not treat its absence as something to "fix."

---

## Codebase-wide compile/correctness pass (2026-07-26)

Decision

Every file in `forge/forge-core/` was reviewed and, where broken, fixed —
this was the first time any of it had actually been compiled. Standalone
`-fsyntax-only` and full runtime tests (under ASan+UBSan) now pass. Fixes
made:

* **`Result.inl` and `Vector.inl`**: both files closed
  `namespace forge::core` far too early (right after the `Failure`
  constructors in `Result.inl`, right after `Data()` in `Vector.inl`),
  silently leaving most of the class's own member definitions (copy/move
  ctors, assignment, `HasValue`/`Value`/`Error`, `Reserve`, `PushBack`,
  etc.) sitting at global scope. Neither file could have compiled. Fixed
  by moving the closing brace to the actual end of each file.
* **`usize` was never declared.** `Allocator.h`, `DefaultAllocator.h/.cpp`,
  and `Vector.h`'s `SizeType` alias all referenced `usize`; `Types.h` only
  defines `Size`. Replaced every occurrence with `Size`.
* **Include-path convention.** Most headers included each other via
  `forge/core/...`, implying a `forge/core/` directory that does not
  exist — the real directory is `forge/forge-core/` (hyphenated, one
  level up). Only `AllocationBackend.h/.cpp` had the right idea (relative
  to the real layout). Standardized everything on relative,
  directory-correct includes. See `PROJECT_CONTEXT.md` → Coding Style.
* **`Vector.h`'s own `.inl` include** pointed at a nonexistent
  `forge/core/containers/Vector.inl`. Fixed to the real, same-directory
  file.
* **`UniquePtr::Reset()`** called `allocator_->Deallocate(pointer_)` with
  one argument; `Allocator::Deallocate` requires `(memory, size,
  alignment)`. Fixed to pass `sizeof(T), alignof(T)`.
* **`UniquePtr`'s destructor** was declared without `noexcept` in the
  header but defined with `noexcept` in the `.inl` — a hard
  declaration/definition mismatch. Header now matches.
* **`Error::OutOfMemory`** (in `MakeUnique.inl` and `Vector.inl`'s
  `Reserve()`) doesn't exist — `Error` is a class with no such static
  member; `OutOfMemory` is an `ErrorCode` enumerator. Fixed to
  `Error(ErrorCode::OutOfMemory)`.
* **`return Failure{...}` from functions returning `Result<X>`** doesn't
  compile — `Result`'s `Failure`-taking constructor is `explicit`. Both of
  the sites above now construct the `Result<X>` explicitly.
* **`ResultVoid.h`/`ResultVoid.inl`** declared and separately defined
  `Ignore()` twice in the same class (hard redefinition error). Removed
  the duplicates.
* **`Vector<T>::EmplaceBack`** was declared in `Vector.h` as returning
  `Result<void>` but implemented in `Vector.inl` as returning `Result<T&>`
  — which is itself ill-formed for any `T`, since `Result<T>`'s storage is
  a `union { T value; Error error; }` and C++ forbids reference members in
  unions. Implementation now matches the header (`Result<void>`).
* **`Result<void>` has no `IsFailure()`** (only `HasValue()`/`HasError()`/
  `operator bool()`); three call sites in `Vector.inl` used it anyway.
  Fixed to `HasError()`. `HasError()` was also added to the primary
  `Result<T>` template for symmetry (purely additive, non-breaking).
* **Member-function name collides with an enclosing-scope type name**:
  `Result<T>::Error()` / `Result<void>::Error()` / `Failure::Error()`
  (returning `Error&`), and `Vector<T>::Size()` (returning `SizeType` =
  `Size`) all hard-error under GCC (`-Wchanges-meaning`) the moment any
  other bare use of the type name appears in the same class. Fixed with
  the `class Error` elaborated-type-specifier for the `Error` cases, and
  by fully qualifying `using SizeType = forge::core::Size;` for the
  `Vector` case (elaborated specifiers don't apply to `using`-aliases).
  The accessor names themselves were **not** renamed — see
  `PROJECT_CONTEXT.md` and `AGENTS.md`.
* **`Vector`'s copy constructor and copy-assignment** silently
  `.Ignore()`d a failing `Reserve()` and then proceeded to construct/copy
  `other.size_` elements regardless — on allocation failure this wrote
  past an empty or undersized buffer (verified as a real crash/UB path
  with a `FailingAllocator` test, not just a theoretical concern). Both
  now bound the copy loop to `min(capacity_, other.size_)`, so a failed
  allocation produces a safely-truncated (empty, in the OOM case) copy
  instead of undefined behaviour.
* **`moz.build`** listed `AllocationBackend.cpp` in `SOURCES` but not
  `DefaultAllocator.cpp` (which calls into it) — the allocator subsystem
  was never actually being built. Added it.
* Minor: removed one dead/unreachable `argc < 2` check in `forge.cpp`
  (unreachable given the `argc == 1` early-return at the top of `main`),
  and fixed `ErrorTests.cpp`'s include to match the path convention above.

Verification

A standalone test translation unit including every forge-core header
compiled cleanly under `g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror`
and `clang++`, and a runtime test (`Result<T>`/`Result<void>` success and
failure paths and their copy/move; `Vector` push/emplace/copy/move/assign
with a non-trivial element type, verifying construction/destruction counts;
`Reserve()`/`PushBack()`/copy-assignment against a deliberately-failing
allocator; `MakeUnique` success and out-of-memory paths) passed under
`-fsanitize=address,undefined` with no leaks, no UB, exit code 0.

Status

Applied. Not yet reflected in any git commit on the user's machine as of
this writing — these are working-tree changes.

Noted but intentionally not changed (flagged for a future decision instead
of a unilateral fix):

* `MakeUnique(Allocator&, Args&&...)` vs `MakeUnique(Args&&...)`: passing a
  custom `Allocator` subclass *by its derived type* (not upcast to
  `Allocator&`) gets silently absorbed by the generic `Args...`-only
  overload instead of routing to the allocator overload, producing a
  confusing "no matching constructor" error instead of the intended
  allocator substitution. Works correctly when the caller passes an
  `Allocator&`-typed reference (the same convention `Vector`'s allocator
  constructor already expects). Fixing this properly needs SFINAE/concepts
  — a real API change, not something to do silently.
* `ResultFwd.h`, `ResultVoid.h`, `ResultVoid.inl` physically live under
  `forge-core/memory/` despite being core `Result` machinery (namespace
  `forge::core`, not `forge::core::memory`) — not memory-specific. Include
  paths were fixed to match their actual location; the files themselves
  were not moved, since that's an organizational call, not a bug fix.
* `Vector::AllocationSize()`/`NextCapacity()` have no overflow checking
  for extreme capacities — a common, known limitation, left as-is to avoid
  scope creep beyond what was asked.

---

## Platform / TimerScheduler (Phase 2, 2026-07-27)

Decision

`forge::core::platform::TimerScheduler` is the first piece of the Phase 2
event-loop rearchitecture (see `ROADMAP.md`). It is deliberately split off
from the actual Windows/IOCP backend: it holds only timer bookkeeping
(`Schedule`, `Cancel`, `Empty`, `Count`, `NextDueDelay`, `PopDue`) over
caller-supplied `u64` millisecond timestamps, with zero OS calls. The
upcoming `IocpLoop` will wrap this and decide how long to block in
`GetQueuedCompletionStatus` based on `NextDueDelay()`, then call `PopDue()`
after waking. Backend selection between platforms will be a compile-time
type alias (`#if defined(_WIN32)`), not a virtual interface — avoids vtable
overhead on a hot path and avoids adding a converting/upcasting constructor
to the frozen `UniquePtr<T>`.

Reason

Everything OS-specific (real IOCP calls) cannot be compiled or tested in
the Linux sandbox this work was verified in, and would need to wait for
the project owner's Windows/Visual Studio machine either way. Splitting
out the pure logic means the part that's actually easy to get subtly wrong
(due-time bookkeeping, safe removal-during-iteration, re-entrant
Schedule/Cancel from inside a firing callback) gets fully verified now
instead of being tangled up with untestable Win32 glue.

Verification

A dedicated test (`TimerSchedulerTest.cpp`, kept alongside the class as a
throwaway harness, not part of the `moz.build` production build) covers:
one-shot timers firing exactly once and being removed; a repeating timer
firing multiple times with correctly-advancing due times, including under
late polling; cancelling a timer before it's due; cancelling a *different*
timer from within another timer's callback, both a still-pending target
and an already-fired-and-removed one; a repeating timer cancelling
*itself* from within its own callback and confirming it does not fire
again; `NextDueDelay` reporting the correct minimum across several
concurrent timers and `false` once none are pending; and scheduling new,
already-due timers from within a callback mid-`PopDue`, forcing the
backing `Vector` to reallocate while the sweep's own index is live.

Passed under `g++`/`clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror`,
under `g++ -fsanitize=address,undefined` (0 errors), and under `valgrind
--leak-check=full` (0 errors, 0 leaks). clang's ASan runtime library was
not installed in this sandbox, so the clang+ASan combination specifically
was not exercised — covered instead by g++ + ASan/UBSan plus valgrind.

Two real bugs were found and fixed during this pass, not just theoretical
review:

* **`Vector.h` never included `DefaultAllocator.h`** despite its default
  member initializer calling `memory::GetDefaultAllocator()` (declared
  there, not in `Allocator.h`, which `Vector.h` *did* include). This had
  never been caught before because every prior compile of `Vector.h`
  happened to have something else include `DefaultAllocator.h` first in
  the same translation unit — an accident of include order, not a
  guarantee. `TimerScheduler.h` including `Vector.h` on its own, with
  nothing else pulling in `DefaultAllocator.h` first, is what exposed it.
  Fixed by adding the include directly to `Vector.h`.
* **`TimerScheduler::PopDue()`'s repeating-timer reschedule anchored the
  next due time to `nowMs`** (the timestamp passed into that `PopDue`
  call) **instead of to the timer's own previous `dueMs`** — the opposite
  of what the header comment documents ("measured from when it was due,
  not from when `PopDue()` happened to run"). Anchoring to `nowMs` lets
  the effective period drift longer every time `PopDue` is called even
  slightly late. Caught by `Test_RepeatingTimerAdvancesDueTime`
  deliberately calling `PopDue` late (at t=23 for a timer due at t=20) and
  asserting the *next* due time is still 30, not 33. Fixed by anchoring to
  `fired.dueMs + fired.delayMs`.

Status

`TimerScheduler.h`/`.cpp` verified and applied.

---

## Platform / IoLoop + IocpLoop (Phase 2, 2026-07-27)

Decision

`forge::core::platform::IoLoop` (`IoLoop.h`) is the compile-time backend
alias called for in `ROADMAP.md` Phase 2 — `#if defined(_WIN32)` selects
`IocpLoop`; any other platform is a hard `#error` until a Linux/macOS
backend is written, rather than silently compiling something broken.

`IocpLoop` (`IocpLoop.h`/`.cpp`) wraps a single Win32 I/O completion port
plus a `TimerScheduler`. It is single-threaded by design — exactly one
thread ever calls `Run()`/`RunOnce()`, matching the single-threaded JS
event-loop model every other engine (V8/Node, JavaScriptCore/Bun) also
uses; blocking or CPU-heavy work belongs on the Phase 7 thread pool, not
here. Async operations are represented by `IoCompletion` (a struct
inheriting from `OVERLAPPED` with an inline `CompletionCallback` function
pointer) that future socket/file code (Phase 5/6) embeds as the first
member of its own per-operation struct — the standard IOCP idiom of
casting the `LPOVERLAPPED` handed back by `GetQueuedCompletionStatus`
straight back to the callback that should handle it, with no virtual
dispatch and no lookup table. `RunOnce()` computes its wait timeout from
`TimerScheduler::NextDueDelay()` so the loop blocks exactly as long as
until the next timer or I/O event, never polling.

Reason

Same reasoning as `TimerScheduler`: get the shape of the loop right once,
now, since every future async API (files, sockets, HTTP) builds on top of
it and retrofitting it later would mean touching all of them.

Verification

The real Win32 calls (`CreateIoCompletionPort`, `GetQueuedCompletionStatus`,
`PostQueuedCompletionStatus`, etc.) cannot be compiled or executed in this
Linux sandbox — there is no Windows SDK here. Two separate checks were
still run, both clearly scoped to what they do and don't prove:

* A minimal mock `windows.h` (stub types/signatures only, every function
  a one-line no-op) type-checked `IocpLoop.cpp` and `IoLoop.h` (with
  `_WIN32` defined) under `g++`/`clang++ -Wall -Wextra -Wpedantic -Werror`.
  This only proves the code is syntactically and type-correct against the
  real Win32 signatures it calls — it says nothing about runtime
  behaviour.
* A second, *stateful* mock `windows.h` backs `GetQueuedCompletionStatus`/
  `PostQueuedCompletionStatus` with a real (fake-OS, single-threaded) FIFO
  queue, so `IocpLoopTest.cpp` could actually drive `IocpLoop`'s own
  control flow end-to-end: a posted completion dispatches on the next
  `RunOnce()` with the right bytes-transferred/completion-key/success
  values; a *failed* completion (operation error, but a real dequeue —
  `GetQueuedCompletionStatus`'s documented third outcome) still dispatches,
  with `succeeded=false` and the right error code, and is not treated as a
  `RunOnce()`-level failure; a due timer fires even when no completion is
  queued (mock "timeout" path); a timer and a completion both firing in
  the same sweep both get handled; a genuine wait failure (as opposed to a
  plain timeout) surfaces as a `Result<void>` error from `RunOnce()`; and
  `RequestStop()` called from inside a completion callback stops `Run()`
  after that iteration, not before or after an extra one. All 7 scenarios
  passed under `g++`/`clang++ -Wall -Wextra -Wpedantic -Werror`, under
  `g++ -fsanitize=address,undefined`, and under `valgrind --leak-check=full`
  (0 errors, 0 leaks in every run). This is real verification of
  `IocpLoop.cpp`'s own logic — the branching on
  `GetQueuedCompletionStatus`'s three outcomes, the `OVERLAPPED*` ↔
  `IoCompletion*` cast, timer/completion interleaving — just not of the
  real OS's behaviour underneath it.

Status

`IoLoop.h`, `IocpLoop.h`, `IocpLoop.cpp` written and logic-verified against
mocks as described above. **Confirmed building and passing on the project
owner's actual Windows/Visual Studio machine (2026-07-27)** via
`IocpSmokeTest.cpp` — real `CreateIoCompletionPort`,
`GetQueuedCompletionStatus`, `PostQueuedCompletionStatus`, a real repeating
timer, and `RequestStop()` all confirmed working against the real Win32
API, not just the mock. `Empty()`/`Count()` (pass-through accessors onto
`TimerScheduler`, needed by the `forge.cpp` integration below to know when
it's safe to stop the loop) were added afterward and are covered by
`IocpLoopTest.cpp` against the mock, but have not themselves been
re-verified on Windows — low risk (trivial one-line delegations), but
worth confirming alongside the `forge.cpp` integration's own build.

---

## forge.cpp — wiring IocpLoop into the real runtime (2026-07-27)

Decision

`forge.cpp`'s old `TimerQueue`/`EventLoop` (busy-poll, 1ms sleep, manual
`now() >= dueTime` scanning) is replaced with
`forge::core::platform::IoLoop`. `setTimeout`/`setInterval`/`clearTimeout`
keep their exact existing JS-facing behaviour (same small integer ids
returned to script) but now schedule through `IoLoop::ScheduleTimer` /
`CancelTimer` instead of a manually-scanned vector. `Runtime::run()`
drains the JS job queue, then blocks in `IoLoop::RunOnce()` for the next
timer or I/O event (currently only timers — real async I/O is Phase 5/6),
repeating until both the job queue and the timer set are empty, instead of
looping with a fixed sleep regardless of whether anything is actually due.

Two id spaces are kept separate on purpose: the small integer
`setTimeout()`/`setInterval()` return to script is unrelated to `IoLoop`'s
own `TimerId`; a `JsTimerRegistry` maps between them.

Reason

This is the actual point of Phase 2 — replacing the wasteful polling loop
this whole rearchitecture was about, now that `IocpLoop` itself is
confirmed working for real.

Two real bugs, unrelated to `IocpLoop` itself, were found and fixed while
writing this integration — not just in review, both were things that would
either fail to compile or corrupt memory if shipped as first drafted:

* **A forward-declaration ordering bug of my own making, caught before
  compiling, not after**: `ForgeTimerFired` (the native-timer-fired
  callback) needs to reach the current `Runtime`'s `JsTimerRegistry` to
  release a fired one-shot timer, but `Runtime` is only fully defined
  later in the file (it embeds `JsTimerRegistry` by value). Fixed with a
  forward-declared `GetRuntimeTimers()` helper, the same pattern the file
  already used for `EnqueueMicrotask`.
* **A real lifetime/memory-safety bug**: the old event loop only ever
  stopped once every timer had already fired or been cancelled, so it
  never needed an early-exit path. The new `run()` can now `break` early
  on a genuine I/O error from `IoLoop::RunOnce()` — and if it does, any
  `JsTimer`s still pending hold a `JS::PersistentRootedVector<JS::Value>`,
  which must unregister itself from `cx` when destroyed. Left alone, those
  would only get destroyed later, inside `Runtime`'s own destructor, which
  runs *after* `JS_DestroyContext(cx)`/`JS_ShutDown()` have already been
  called at the bottom of `main()` — a destroy-after-teardown bug. Fixed
  by adding `Runtime::Shutdown()` (cancels and releases every pending
  timer) called explicitly, while `cx` is still alive, right before
  `JS_DestroyContext(cx)` — and also on the one other path that can leave
  timers pending before reaching that point (a script that calls
  `setTimeout` and then throws during top-level evaluation).

Also fixed in passing, since it directly concerns the exact callback
objects this rewrite revolves around: **neither `Timer::callback` nor
`Microtask::callback` (`JS::Heap<JSObject*>`) was ever traced by the
GC.** `JS::PersistentRootedVector` (used for `arguments`) self-registers
and needs no manual tracing, but a bare `JS::Heap<T>` member does — without
a trace hook, a GC that runs while a timer or microtask is still pending
could collect its callback out from under it. Fixed by registering a
`JS_AddExtraGCRootsTracer` callback (`TraceForgeRoots`) that traces every
live timer's and microtask's callback. This bug pre-dates this change
entirely (it's been there since the original prototype) — it was only
found now because rewriting the timer storage was the moment to look at it
closely.

Verification

**This could not be compiled or tested in this sandbox at all** — unlike
`IocpLoop.cpp`, where a small, precisely-known Win32 API surface (about 10
functions) could be faithfully mocked, `forge.cpp` calls deeply into
SpiderMonkey's own API (`JS::Call`, `JS::CallArgs`, rooting, realms,
compilation, GC tracing), a much larger and more version-sensitive
surface. Building a mock of that would risk asserting false confidence
rather than providing real signal, so none was attempted. What *was* done:
a full manual re-read of the rewritten file for control-flow ordering,
lifetime, and GC-safety (which is how the two bugs above were caught
before ever reaching the project owner), reusing the exact call shapes
already proven to compile in the pre-existing code (e.g. the
`JS::Call(cx, thisValue, callback, timer->arguments,
JS::MutableHandleValue(&rval))` shape is copied verbatim from the working
original), and a brace/paren balance check. The two genuinely new
SpiderMonkey API calls introduced here — `JS_AddExtraGCRootsTracer` (in
`js/GCAPI.h`) and `JS::TraceEdge` (in `js/TracingAPI.h`) — are the most
likely spots for a real build to disagree with, if anything does.

Status

Written, not yet built. **Needs a real build on the project owner's
Windows/Visual Studio machine** — same as every other unverifiable-here
change, please report back exactly what the compiler says if anything
doesn't build, especially around the two new GC-tracing calls called out
above.
