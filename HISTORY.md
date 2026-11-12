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

---

## Real build environment discovered: C++17, no exceptions, not C++20 (2026-07-27)

Decision

The first real `mach build` attempt (via the actual Gecko/SpiderMonkey
tree at `js/src/forge`, not a standalone Visual Studio project) revealed
that this build target is **not** what every prior verification in this
project assumed. Every earlier "compiles cleanly" claim in this file used
`g++`/`clang++ -std=c++20` in the sandbox, and the standalone Visual
Studio smoke tests (`VectorSmokeTest.cpp`, `IocpSmokeTest.cpp`) apparently
used a project configuration with C++20 and exceptions both enabled. The
real `mach build`, however:

* Compiles this codebase as C++17, not C++20 (evidenced by
  `-Wc++20-extensions` warnings on things that are only valid in C++20).
* Disables C++ exceptions entirely (`error: cannot use 'try' with
  exceptions disabled`) — not a style preference, a hard compiler
  configuration that makes `try`/`catch`/`throw` a compile error wherever
  they appear unconditionally.

This is a real, load-bearing fact about the project now: **forge-core
must compile as strict C++17, with exceptions disabled, in the real
build** — even though a standalone Visual Studio project pointed at the
same files might successfully compile more permissive C++20-or-exceptions
code and never reveal the mismatch. From here on, verification in this
sandbox uses `-std=c++17 -fno-exceptions` to actually match, instead of
`-std=c++20`.

Six real bugs, spanning most of forge-core, were caused by this mismatch
and are now fixed:

* **`ResultStorage.h`'s union constructor/destructor were `constexpr`.** A
  constexpr destructor (and a constexpr union constructor that doesn't
  initialize a member) are both C++20-only. Neither one is ever needed in
  a constant-evaluated context anywhere in this codebase, so `constexpr`
  was simply removed from both.
* **`Error.h`'s `operator==` used `= default`** on a non-member/friend
  comparison operator — C++20's "defaulted comparisons" feature. Replaced
  with an explicit, hand-written member-wise comparison (same semantics,
  valid since C++11).
* **`std::construct_at` (C++20, `<memory>`) was used 19 times** across
  `Result.inl`, `Vector.inl`, and `MakeUnique.inl`. Added a small
  C++17-compatible replacement, `forge::core::detail::ConstructAt()` (new
  file, `Construct.h` — plain placement-new under the hood, same call
  shape as `std::construct_at` so every call site is a mechanical
  substitution) and switched every call site to it.
* **`memory/detail/AllocationBackend.cpp` used `std::has_single_bit`**
  (C++20, `<bit>`) to check that an alignment is a power of two. Replaced
  with the equivalent bit trick (`(alignment & (alignment - 1)) == 0`),
  which is plain C++11.
* **The same file wrapped the actual allocation in an unconditional
  `try`/`catch (const std::bad_alloc&)`**, which cannot compile at all
  with exceptions disabled — not merely non-idiomatic for a
  zero-exception codebase (see `AGENTS.md`), a hard build failure.
  Replaced with the standard non-throwing overload,
  `::operator new(size, align_val_t, std::nothrow)` (C++17), which needs
  no exception handling at all. `MakeUnique.inl`'s own `try`/`catch` was
  **not** touched beyond the `construct_at` fix — it was already correctly
  guarded behind `#if defined(__cpp_exceptions)` (that macro is
  undefined when exceptions are disabled, exactly as it should be), so it
  already compiles correctly in both configurations; it just also needed
  the same `construct_at` fix as everywhere else.

Verification

Every fix was re-verified the same rigorous way as before, but now under
the conditions that actually matter — `-std=c++17 -fno-exceptions` — not
`-std=c++20`:

* `TimerScheduler` + its full test suite: g++ and clang++, strict warnings
  (including `-Wc++20-extensions` explicitly enabled, to catch anything
  else relying on a C++20 extension), and `-fsanitize=address,undefined`.
  All 7 scenarios still pass.
* `IocpLoop` + its full mock-based test suite: same treatment, all 8
  scenarios still pass.
* `Vector<T>`: a fresh standalone check (push/emplace/copy/move on both a
  trivial and a non-trivial element type) under the same flags, plus ASan.
* `MakeUnique<T>`: a fresh standalone check (success path, value
  correctness) under the same flags, plus ASan.

All four passed clean, with zero warnings, under the real constraints this
time.

Status

Fixed. This is a significant finding beyond just these six bugs: it means
the actual build environment (Gecko's `mach build`) is meaningfully
stricter than anything verified here or in a standalone Visual Studio
project until now. Any future forge-core code — written by any AI agent
per `AGENTS.md`, not just this fix — needs to be written as plain C++17
with no exceptions from the start, not C++20-with-exceptions code that
happens to also compile in a more permissive standalone test project. This
should be checked into `AGENTS.md` as a hard constraint, not just this
history entry.

---

## First successful real `mach build` of the new event loop (2026-07-27)

Decision / Milestone

`python mach build`, run for real from `C:\spidermonkey-dev\gecko-dev`,
succeeded: `js/src/forge/forge.exe` now builds cleanly with `TimerScheduler`,
`IocpLoop`, `IoLoop`, and the rewired `forge.cpp` all included, after the
C++17/no-exceptions fixes above. Output binary lands at
`obj-spider\dist\bin\forge.exe` (per `build.bat`/`install.ps1`'s existing
copy step), not directly under `js/src/forge`.

This is the first time any of Phase 2's actual code has been built by the
real toolchain end to end — everything before this was either sandbox
verification (g++/clang++ against mocks) or a standalone Visual Studio
smoke test (`IocpSmokeTest.cpp`) that only covers `IocpLoop` in isolation,
not `forge.cpp`'s integration of it into the real JS runtime.

Status

Builds successfully. **Functionally verified against real `hello.js`
(2026-07-27):** `forge.exe hello.js` printed `hello` exactly once (one-shot
timer fires once, doesn't repeat), then `tick` repeatedly with no crash
(the uncancelled `setInterval` keeps firing correctly through the new
loop), and never printed `never` (the timer `clearTimeout`'d before it was
due was genuinely cancelled). This is real confirmation that
`setTimeout`/`setInterval`/`clearTimeout` work correctly end-to-end
through `IocpLoop` — not just that the code compiles. Running long enough
to see many repeated ticks without a crash is also reasonable (if not
airtight) evidence the GC-tracing fix is holding up under whatever GC
pressure SpiderMonkey's normal allocation-triggered collection produced
during the run; a more deliberate GC-pressure test would need a way to
force a collection from script, which isn't exposed yet.

Still open: the 47 compiler warnings from the build haven't been reviewed
to confirm none originate from forge/forge-core's own files.

---

## Crash on every finite script: `global` destroyed after `JS_DestroyContext` (2026-07-27)

Decision / Bug

Running the Phase 0 benchmark suite (`run-benchmarks.ps1`) for the first
time against the real `forge.exe` crashed on all three scripts
(`startup.js`, `json-bench.js`, `loop-bench.js`), every time, with exit
code `-1073741819` (`0xC0000005` — access violation). This is **not**
related to timers, `IocpLoop`, or anything from this session's event-loop
work — none of the three scripts schedule a single timer.

Root cause: `JS::RootedObject global` was declared at the top level of
`main()`, alongside `cx`/`runtime`, so its destructor only runs when
`main()` itself returns — which is *after* `JS_DestroyContext(cx)` and
`JS_ShutDown()` have already executed a few lines earlier.
`JS::Rooted<T>`'s destructor has to unlink itself from a list owned by the
context it was rooted against; with that context already destroyed, this
is a genuine use-after-free, matching the observed access violation
exactly.

This bug **pre-dates every change made this session** — `global` was
declared at that same scope in the very first version of `forge.cpp` ever
read in this project. It had simply never been triggered, for a subtle
reason worth recording: no script had ever been run to natural completion
through a real `mach`-built `forge.exe` before now. `hello.js` (the only
prior real-build test) schedules an uncancelled `setInterval`, so `run()`
never returns on its own — the user always had to Ctrl+C it, which kills
the process immediately without ever running C++ destructors or reaching
`JS_DestroyContext`. The Phase 0 benchmark scripts are finite (no pending
timers), so they're the first scripts to ever actually reach the bottom of
`main()` — and that's exactly what exposed this.

Fix

Wrapped `global`'s declaration (and everything using it, including the
existing `JSAutoRealm` block) in a new nested scope that closes *before*
`runtime.Shutdown()`/`JS_DestroyContext(cx)`/`JS_ShutDown()` run, instead
of after. No other logic changed.

Verification

**Could not be compiled or run here** — same SpiderMonkey-API-surface
constraint as the rest of `forge.cpp` (see the C++17/no-exceptions entry
above): the API surface is too large to mock faithfully, so no sandbox
verification was attempted. What gives this fix unusually high confidence
despite that: the crash signature (access violation, only on scripts that
reach normal completion, first appearing on the very first finite scripts
ever run) matches this exact, well-known SpiderMonkey embedding pitfall
precisely, and the fix is a pure scope/ordering change — no new API calls,
no new logic, just moving where a brace closes.

Status

Fixed and confirmed on the project owner's machine (2026-07-27) —
`run-benchmarks.ps1` now runs all three scripts to completion with no
crash, on both the old busy-poll path and (implicitly) whatever code path
these scripts exercise now. Diagnosis was correct.

---

## Phase 0 baseline established: first real Forge vs Bun vs Node numbers (2026-07-27)

Result (from `run-benchmarks.ps1`, 5 iterations each, see
`bench/results/2026-07-27_162925.csv` for the raw data):

| benchmark     | forge (avg) | bun (avg) | node (avg) | forge/bun ratio |
|---------------|-------------|-----------|------------|-----------------|
| startup.js    | 25.01 ms    | 47.35 ms  | 53.85 ms   | **0.53x**       |
| json-bench.js | 383.29 ms   | 264.53 ms | 566.12 ms  | 1.45x           |
| loop-bench.js | 89.96 ms    | 81.23 ms  | 103.21 ms  | 1.11x           |

Reading

`startup.js` measures process cold-start — engine init, global/realm
setup, the event loop coming up — which is exactly the layer Phase 2's
work (and Forge's minimal footprint generally, vs. Bun's much larger
feature set at startup) actually controls. **Forge starts in about half
the time Bun does.** This is the first real evidence the architecture
decisions so far (SpiderMonkey + the lean event loop rewrite) are paying
off where they're supposed to.

`json-bench.js` and `loop-bench.js` are the two benchmarks explicitly
flagged in their own source comments as measuring the underlying JS
*engine* (SpiderMonkey vs JavaScriptCore), not anything Forge's own
runtime layer currently touches — Forge hasn't done any engine-level or
built-in-fast-path work yet (that's later phases, if ever, per
`ROADMAP.md`). Unsurprisingly, stock SpiderMonkey trails stock
JavaScriptCore by 11–45% on these; what's notable is Forge still clearly
beats Node on both (32% faster on `json-bench.js`, 13% faster on
`loop-bench.js`), and the startup win suggests there's real headroom in
what Forge's *own* layer can still contribute once Phase 1
(String/Span), Phase 3 (allocator hardening), and Phase 6 (native
networking/HTTP fast paths) land on top of this foundation.

Status

Phase 0 is now genuinely complete: real baseline numbers exist, not just
benchmark scripts sitting unused. This is the number to track as every
later phase lands — especially whether `json-bench.js`/`loop-bench.js`
close the gap with Bun as forge-core's own allocator/collection work
matures, since those two are the ones current work can plausibly still
move.

---

## Span / StringView / String (Phase 1, 2026-07-27)

With Phase 0 (baseline) and Phase 2 (event loop) both landed, the next
piece of `ROADMAP.md`'s own Phase 1 — `String`/`StringView`/`Span` — was
implemented: the last of the "not started" core containers listed in
`PROJECT_CONTEXT.md`.

`Span<T>` is a generic non-owning view (`Data`/`Size`/`Empty`/
`operator[]`/`Front`/`Back`/`Subspan`, the last clamped so an
over-generous offset/count can't read out of bounds — the common "give
me the rest" mistake via a huge count shouldn't be UB). `StringView` is
built on `Span<const char>` rather than duplicating its bounds logic,
adding the string-specific vocabulary (`Substr`/`StartsWith`/`EndsWith`/
`Find`/comparisons/implicit construction from a null-terminated
`const char*`). `String` is the owning, growable, `Allocator`-based
counterpart, deliberately mirroring `Vector<T>`'s exact growth/`Reserve`/
`GrowIfNeeded`/copy-truncates-safely-on-OOM conventions, but always
maintaining a null terminator internally (`AllocationSize(capacity) =
capacity + 1`) so `CStr()` is always valid — including on a
default-constructed String that has never allocated — for interop with
C-style APIs. Construction never allocates; `String::Create(allocator,
initial)` / `String::Create(initial)` are the fallible factories for a
String that already holds content, per the decision already recorded
above this entry.

Verified with a new `StringTest.cpp` (13 scenarios: happy-path
construction/append/copy/move/assignment, cross-type comparisons, and
three allocator-failure paths via a `FailingAllocator`, mirroring
`Vector<T>`'s own OOM-test precedent) under the real project
constraints established in the C++17 entry above: `g++`/
`clang++ -std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic -Werror`
(plus `-Wc++20-extensions` on clang), then re-run clean under
`-fsanitize=address,undefined` and `valgrind --leak-check=full` (0
errors, 21 allocs/21 frees, no leaks possible), plus an `-O2` pass for
good measure. Not yet retested via a real `mach build` — these are pure
header/`.inl` files with no SpiderMonkey dependency, so this sandbox
verification is materially more trustworthy than the `forge.cpp`/
event-loop work's was, but per `AGENTS.md` that's still not "confirmed"
until it's actually gone through Gecko's build.

Two real bugs, caught by this verification rather than by inspection:

* **`String.h` never included `Result.h`.** It included
  `memory/ResultVoid.h` (for the `Result<void>` specialization) but not
  the primary `Result<T>` template's own header, relying only on the
  forward declaration in `ResultFwd.h`. That's enough for a function
  *declaration* like `static Result<String> Create(...)`, but
  `String.inl`'s function *bodies* actually construct and return a
  `Result<String>`, which requires the primary template to be a
  complete type. Fixed by adding `#include "Result.h"` to `String.h`.
* **`String == String` did not compile**, despite a comment in `String.h`
  confidently asserting it would "just work" via the implicit
  `String -> StringView` conversion plus `StringView`'s own
  `operator==`. It doesn't: `StringView::operator==` is a *hidden
  friend* (declared only inside the class body, no matching
  namespace-scope declaration), and hidden friends are only found by
  argument-dependent lookup when the declaring class itself
  (`StringView`) is literally one of the call's argument types — not
  merely reachable via an implicit conversion from a *different* class
  in the same namespace. Since `String == String` has no `StringView`
  argument at all, `StringView`'s comparison operator was never
  considered, and gcc rejected it outright
  ("no match for 'operator=='"). Fixed by giving `String` its own three
  hidden-friend overloads (`String==String`, `String==StringView`,
  `StringView==String`), each delegating to `.View() == ...`; a bare
  `String == "literal"` still resolves correctly through these, since
  the literal has no class type to constrain ADL and the `String`
  operand pulls the friend in regardless. The stale comment claiming the
  old approach worked was removed.

Separately, while staging these new files to the device, the two
tracked copies of this repo were found to have drifted: the C++17/
no-exceptions fixes from the entry above (`Construct.h`, and the fixes
to `Error.h`, `Result.inl`, `ResultStorage.h`, `memory/MakeUnique.inl`,
`memory/Vector.inl`, `memory/detail/AllocationBackend.cpp`) had only
ever been written to `C:\spidermonkey-dev\gecko-dev\js\src\forge\`
(the real `mach build` location) and never mirrored back to
`C:\forge-beta\forge-runtime-beta\` — so the latter's `forge-core` was
silently back to the pre-fix, C++20-assuming state. Synced all seven
files back to `C:\forge-beta\forge-runtime-beta\` alongside this
entry's new files, so both locations match again. Worth remembering:
this repo effectively has two live copies with no automatic sync
between them, and it's easy for a fix delivered to only one of them to
silently go stale in the other.

Status

`Span`/`StringView`/`String` implemented and verified (sandbox, not yet
`mach build`). `ROADMAP.md`'s Phase 1 is marked done. Next candidates
per `ROADMAP.md`: Phase 2 (Array/HashMap/HashSet/Queue/Stack), or,
per the broader runtime roadmap referenced elsewhere in this file,
Phase 3 (allocator hardening) or reviewing the 47 `mach build` warnings
that have never been triaged (still outstanding — the warning text has
not yet been provided).

---

## Pre-work review: forge.cpp/moz.build drift, a real missing-sources bug (2026-07-29)

Before starting Phase 2, reviewed the current state of both tracked
copies (`C:\forge-beta\forge-runtime-beta\` and
`C:\spidermonkey-dev\gecko-dev\js\src\forge\`) against `ROADMAP.md`, per
this session's own instructions to check for drift/duplication before
adding new code. Found two real, separate problems:

**`forge.cpp` and `moz.build` were never synced back to `forge-beta`.**
The prior session's entry above only says `forge.cpp` (the full
IocpLoop-rewrite/crash-fix version) was delivered to the `gecko-dev`
build location — unlike the `forge-core/` files, it was never mirrored
to `forge-beta`. Confirmed via `diff`: `forge-beta`'s copy was still the
pre-rewrite prototype (`std::vector<std::unique_ptr<Microtask>>`, no
`IoLoop`, no crash fix). Synced the current `gecko-dev` version back.

**`moz.build`, independently, was missing two `SOURCES` entries it
needs.** The live `moz.build` at the real build location listed
`forge-core/memory/DefaultAllocator.cpp`,
`forge-core/memory/detail/AllocationBackend.cpp`, and `forge.cpp` —
but not `forge-core/platform/IocpLoop.cpp` or
`forge-core/platform/TimerScheduler.cpp`, even though `forge.cpp`
`#include`s `IoLoop.h` and directly constructs/calls into
`forge::core::platform::IoLoop` (== `IocpLoop` on Windows), and neither
file is header-only. This entry's earlier prose says these two lines
were added and a `mach build` succeeded with them present — so at some
point after that success, `moz.build` reverted to a 3-entry `SOURCES`
list missing both platform files, most likely from stale linked object
files papering over the missing sources on an incremental build rather
than a clean one. However it happened, a `mach clobber` or a build on a
clean checkout would almost certainly have hit a linker error for
undefined `IocpLoop`/`TimerScheduler` symbols. Added both lines back to
`moz.build` and synced the fix to both locations. Worth a rebuild to
confirm before relying on it further.

Also noticed (lower priority, fixed while here): `forge/CMakeLists.txt`
(a local, non-`mach`, IDE/CMake build for `VectorTest`) still set
`CMAKE_CXX_STANDARD 20` — exactly the assumption the C++17 entry above
found to be wrong for the real build. Changed to 17 so building
`VectorTest` locally via CMake doesn't mask a C++20-only construct that
`mach build` would reject, the same trap that caused the whole C++17
investigation in the first place.

No code changes resulted from this review beyond the sync/fixes above —
`Array`/`HashMap`/`HashSet`/`Queue`/`Stack` were confirmed genuinely not
started (no files present at either location), so Phase 2 work below
does not duplicate anything.

---

## Array / Hash / Stack / Queue / HashMap / HashSet (Phase 2, 2026-07-29)

Implemented all five `ROADMAP.md` Phase 2 components, plus a new `Hash<T>`
trait (`Hash.h`) that `HashMap`/`HashSet` need but that didn't fit neatly
under any single one of the five names.

`Array<T, N>` is the one deliberate departure from every other
container's private-members-plus-named-accessors convention: it's a
plain aggregate with a public `T data_[N]`, matching `std::array`'s own
reasoning — a fixed-size array has no size/capacity relationship to
protect (unlike `Vector<T>`, where `size_ <= capacity_` is exactly the
invariant the private members exist to guard), so there's no invariant
for an accessor to be defending. `N == 0` needed a separate template
specialization (holding no storage at all) since a zero-length C array
isn't valid C++.

`Stack<T>` is a thin LIFO adapter directly over `Vector<T>` — no new
storage logic. `Queue<T>` is a real circular buffer rather than a second
`Vector`-backed adapter: a `Vector`-backed FIFO would need an O(n) shift
on every `Pop()` to slide the remaining elements down, so `Queue<T>`
tracks its own `head_` and wraps indices with modulo, same
`Allocator`/`Reserve`/copy-truncates-on-OOM conventions as `Vector<T>`
otherwise (growth linearizes the circular layout back to starting at
index 0 in the new buffer).

`Hash<T>` (new) is FNV-1a for byte ranges (`StringView`/`String`) and a
splitmix64-style finalizer for integers before they're masked to a table
index — plain identity hashing of small sequential integers (0, 1, 2,
...) would cluster badly against the power-of-two table sizes `HashMap`
uses. The primary template is deliberately left undefined rather than
falling back to hashing a type's raw bytes: a key type without an
explicit `Hash<K>` specialization should fail to compile with a clear
"no such specialization" error, not silently hash padding bytes or fail
confusingly deep inside `HashMap`'s internals.

`HashMap<K, V>` is open addressing with linear probing and tombstones
for deletion (find scans through tombstones; insert reuses the first one
seen along the probe sequence) — chosen over a more elaborate
Robin-Hood/SwissTable-style design specifically because
`PROJECT_CONTEXT.md` calls out "readability over cleverness," and linear
probing with tombstones is the version of open addressing most people
can read and verify by eye. Table capacity is always a power of two so
the index is `hash & (capacity - 1)` (masking) rather than a modulo;
growth doubles capacity once `size + tombstones` crosses 75%, counting
tombstones because a table full of tombstones is exactly as slow to
probe as one full of live entries. Copy construction/assignment reuse
`Insert()` itself (rather than a raw indexed copy loop the way
`Vector`/`String` do it) — `Reserve()` best-effort up front, then insert
every source entry, stopping early if `Insert()` starts failing; this
reaches the same truncate-safely-on-OOM outcome as the rest of the
codebase's containers, just via composition instead of manual memory
copying. Deliberately does not (yet) support a custom hasher/
key-equality template parameter, or const iteration — both are ordinary
additions when a concrete caller actually needs them, left out for now
per "small, clean API."

`HashSet<K>` is a thin wrapper around `HashMap<K, detail::Unit>` (a new
empty tag type), not a second, independent table implementation — any
future correctness fix or improvement to `HashMap`'s probing/growth
applies to `HashSet` automatically, at the cost of one `Unit`-sized slot
per entry that a hand-written set wouldn't need to store. Considered
building both on top of a single shared internal template instead
(avoiding even that overhead), but `V = void` isn't legal C++ and the
`[[no_unique_address]]` trick that would elide it outright is a C++20
feature this codebase can't use — the composition-over-`HashMap`
approach was the simplest one that stayed within the C++17 constraint.

Verified with five new test files — `ArrayTest.cpp`, `StackTest.cpp`,
`QueueTest.cpp`, `HashMapTest.cpp` (which also covers `Hash<T>`
directly), `HashSetTest.cpp` — 34 scenarios in total, including: a
1000-entry insert/find/erase/rehash stress test for `HashMap` (confirms
growth and tombstones interact correctly at scale, not just for a
handful of hand-picked keys); a `Queue` wraparound test that pushes/pops
through the same four physical slots repeatedly to make sure the
circular indexing is actually exercised, not just growth-then-drain; and
allocator-failure paths for every container via the same `FailingAllocator`
pattern `StringTest.cpp` established, confirmed to leave each container
in a safe, still-usable (if smaller/emptier than requested) state rather
than corrupting anything.

All five compiled clean on the first attempt under the full verification
bar established for Phase 1 — `g++`/`clang++ -std=c++17 -fno-exceptions
-Wall -Wextra -Wpedantic -Werror` (plus `-Wc++20-extensions` on clang),
`-fsanitize=address,undefined`, `valgrind --leak-check=full` (0 errors,
0 leaks across all five binaries), and an `-O2` pass. Unlike Phase 1 (and
every prior real-build pass), this round did not surface any bugs the
initial design missed — worth noting honestly rather than inventing a
finding, per `AGENTS.md`'s "Be Honest": it means the design held up under
this level of scrutiny, not that no bug could possibly exist. As with
Phase 1, this is pure header/`.inl` code with no SpiderMonkey dependency
and no `moz.build` changes needed (test files are not added to
`SOURCES`, matching the established `*Test.cpp` convention) — still not
"confirmed" until it goes through a real `mach build`, per `AGENTS.md`.

Status

`ROADMAP.md`'s Phase 2 is marked done. Both tracked copies
(`forge-beta` and the real `gecko-dev` build location) now have matching
`forge-core/`, `forge.cpp`, and `moz.build`. Next per `ROADMAP.md`:
Phase 3 (Filesystem) — note its own entry requires freezing a spec doc
first, matching `Error.md`'s shape, before writing the public API. Still
outstanding from before: the 47 `mach build` warnings have never been
triaged (text still not provided), and a real `mach build`/benchmark
rerun would be worth doing now that forge.cpp/moz.build drift has been
fixed, to confirm nothing regressed.

---

## `forge-beta` retired; `gecko-dev` is now the only tracked copy (2026-07-29)

The user confirmed everything through Phase 2 on their own machine (real
`mach build`, benchmarks, the shutdown-crash fix) and gave a new standing
instruction: work only in `C:\spidermonkey-dev\gecko-dev\js\src\forge\`
from here on; don't develop in or sync to `C:\forge-beta\forge-runtime-beta\`
unless explicitly asked. Also reconfirmed as a standing rule (not new,
but worth restating since it's now the *only* rule): always verify new
code under the real `mach build`'s exact constraints (C++17,
`-fno-exceptions`) from the start, not a more permissive setup first.

This retires the two-copies-can-drift problem the last two entries spent
real effort on (the `forge.cpp`/`moz.build` sync gap, the missing
`SOURCES` entries) — there's only one copy to keep consistent now.

Since `forge-beta` is retired, `HISTORY.md`/`ROADMAP.md`/
`PROJECT_CONTEXT.md`/`AGENTS.md` needed a new writable home:
`.claude\` at the `gecko-dev` location still rejects remote writes (see
the Phase 1 entry above), so all four now live directly under
`js\src\forge\` itself, alongside `forge-core/`/`forge.cpp`/`moz.build`.
`PROJECT_CONTEXT.md` gained a short "Source of truth" note at the top
pointing at this.

Small fix made in passing while reviewing `Error.h` for Phase 3 (see
below): `Error.md`'s frozen spec lists `PlatformError` as one of
`ErrorCode`'s categories, but the actual `ErrorCode` enum in `Error.h`
never had it — a real implementation-vs-frozen-spec mismatch, not a new
design decision, so adding it back is a bugfix per `AGENTS.md` ("a frozen
API that does not compile [or doesn't match its own spec] is a bug, not
a design freeze"). Added at the end of the enum (preserves existing
numeric values) with a comment explaining why it was missing. This
value is what Phase 3's `File` needed anyway, to carry a raw Win32 error
code that doesn't map onto any of the existing generic categories.

---

## Path / File (Phase 3, 2026-07-29)

Implemented Phase 3 following its own stated process: froze a spec doc
first. Actually two — `Path.md` and `File.md` — rather than one combined
"Filesystem" doc, because `Path` and `File` turned out to be different
enough in kind (a pure value type with zero OS dependency vs. a type
whose entire job is calling the OS) that splitting them mirrors the
`StringView`/`String` relationship: one half never touches the OS, the
other is entirely the OS. This split is itself the main design decision
this phase made — recorded in both spec docs' own "Purpose" sections.

**`Path`** stores a path as a `String` (UTF-8, `/`-normalized internally
regardless of whether the input used `/` or `\`) and provides Join,
Parent, FileName, Stem, Extension, IsAbsolute/IsRelative (Windows drive-
letter or UNC absolute; a bare leading `/` is deliberately NOT absolute,
matching `std::filesystem::path`'s own behavior on Windows), and
Normalize (collapses `.`/repeated separators, resolves `..` against a
real preceding segment where possible, never escaping above a leading
`..` in a relative path or above an absolute root). Zero OS calls
anywhere in `Path.h`/`Path.inl` — verified that this stayed true by
literally checking neither file mentions any platform header.

Added `StringView::RFind` (last-occurrence search, mirroring the
existing `Find`) as a small backward-compatible extension — `Parent()`/
`FileName()` need to find the *last* path separator, and `StringView`
was never declared frozen anywhere the way `Error` was, so this is an
addition, not a redesign of anything.

Verified `Path` with `PathTest.cpp` (11 scenarios, including allocator-
failure paths and a set of hand-traced `Normalize()` cases — absolute
`..`-above-root, relative `..` with nothing left to cancel against,
`.`-only paths, mixed/repeated separators) under the real project
constraints from the very start this time (per the user's new standing
instruction): `g++`/`clang++ -std=c++17 -fno-exceptions -Wall -Wextra
-Wpedantic -Werror` (+ `-Wc++20-extensions` on clang), clean under
ASan+UBSan and `valgrind --leak-check=full` (0 leaks, 79 allocs/79
frees), plus an `-O2` pass. All green on the first attempt — same
honest caveat as Phase 2's equivalent note: this means the design held
up under this scrutiny, not that no bug could exist.

**`File`** (`Open`/`Read`/`Write`/`Seek`/`Tell`/`SizeInBytes`/`Close`,
plus static `Exists`/`CreateDirectory`/`CreateDirectories`/`Remove`/
`ReadAllBytes`/`ReadAllText`) is Win32-only and synchronous by design —
`File.md`'s Non-Goals section explains why async is deliberately
out of scope (it would couple this component to `IocpLoop` and to
whatever the eventual JS-visible `fs` API looks like, which is Runtime
Integration's job, not this phase's). Move-only (`HANDLE` has no
sensible cheap copy), matching `UniquePtr<T>`'s ownership model rather
than `Vector<T>`'s. Every Win32 failure is translated to
`Error(ErrorCode::PlatformError, <raw GetLastError() value>)` unless a
more specific code clearly applies (`NotFound` for
`ERROR_FILE_NOT_FOUND`/`ERROR_PATH_NOT_FOUND`, `PermissionDenied` for
`ERROR_ACCESS_DENIED`, `AlreadyExists` for `ERROR_FILE_EXISTS`/
`ERROR_ALREADY_EXISTS`) — never a raw `BOOL`/`GetLastError()` leaking
out. `File.h` keeps the actual `HANDLE` behind a `void*` so it never
needs to `#include <windows.h>` itself, matching `IocpLoop`'s own
reasoning for keeping its `OVERLAPPED`-derived types out of its public
header.

**This component could not be compiled or executed in this sandbox at
all** — there is no Windows SDK here, and installing a MinGW cross
compiler failed (`apt-get install g++-mingw-w64-x86-64` — the package
lives in Ubuntu's `universe` component, which returned 403 Forbidden;
this sandbox's network access is allowlisted to package registries, not
arbitrary Ubuntu mirrors/components). Verification here was: careful
manual review against the real Win32 API shape from memory/knowledge of
`CreateFileW`/`ReadFile`/`WriteFile`/`SetFilePointerEx`/`GetFileSizeEx`/
`CloseHandle`/`GetFileAttributesW`/`CreateDirectoryW`/`DeleteFileW`/
`RemoveDirectoryW`/`MultiByteToWideChar`, plus a hand-written,
type-check-only mock `windows.h` (same technique the `IocpLoop` mock
used last phase) with accurate real signatures. `File.cpp` compiled AND
linked cleanly against that mock under the full `-std=c++17
-fno-exceptions -Wall -Wextra -Wpedantic -Werror` bar on both compilers
— confirming no typos, wrong argument counts, or wrong types anywhere,
but proving nothing about actual runtime correctness (the mock's
functions are dumb stubs — e.g. `MultiByteToWideChar` always returns 0,
so running the mock-linked binary predictably fails almost every check;
that's expected noise from the stub, not a signal about `File.cpp`
itself). A real bug this review pass did catch before it could ship: an
early draft of `CreateDirectories()` collected ancestor directories as
`StringView`s aliasing a `Path` variable that then got reassigned each
loop iteration — since `Path`'s (i.e. `String`'s) assignment can
reallocate/free the old buffer, those views would have dangled. Fixed by
collecting owned `Path` copies (`Vector<Path>`) instead of views.

Wrote `FileSmokeTest.cpp` (matching `IocpSmokeTest.cpp`'s precedent) —
a real-filesystem test exercising `CreateDirectories`, `Exists`,
`Open`/`Write`/`Read` round-tripping through actual file content,
`Append` positioning at end-of-file, `Seek`/`Tell`, `CreateNew`
correctly failing with `AlreadyExists` against a file that already
exists, opening a missing file correctly reporting `NotFound`, and
cleanup via `Remove`. This is **the next thing to run on the real
machine** — it cannot be exercised meaningfully anywhere else.

`moz.build` updated: added `forge-core/File.cpp` to `SOURCES` (`Path`
is header-only, needs no new `.cpp` entry, matching `String`/`Span`'s
precedent). `FileSmokeTest.cpp`/`PathTest.cpp` are not added, matching
the established `*Test.cpp`/`*SmokeTest.cpp` convention.

Status

`ROADMAP.md`'s Phase 3 is marked done for `Path` (fully verified) but
explicitly NOT confirmed for `File` — implemented and reviewed as
carefully as this environment allows, but genuinely unverified until a
real `mach build` (or a standalone Visual Studio build of
`FileSmokeTest.cpp`, matching `IocpSmokeTest.cpp`) runs on the actual
machine. This needs to be communicated to the user explicitly, not
glossed over, per `AGENTS.md`'s "Be Honest" and the user's own explicit
request this session to be told when something can't be fully verified
without a real build. Next per `ROADMAP.md`: Phase 4 (Threading).

## `File::CreateDirectory` renamed to `MakeDirectory` — real `mach build` bug (2026-07-29)

The user ran the actual `mach build` and it failed compiling
`File.cpp` with three errors, all rooted in one cause:

```
File.cpp(388,20): error: out-of-line definition of 'CreateDirectoryA'
does not match any declaration in 'forge::core::File'; did you mean
'CreateDirectory'?
    ...
fileapi.h(67,26): note: expanded from macro 'CreateDirectory'
    67 | #define CreateDirectory  CreateDirectoryA
```

`<windows.h>`'s `fileapi.h` `#define`s `CreateDirectory` to
`CreateDirectoryA` (or `CreateDirectoryW` under a `UNICODE` build) —
the classic Win32 ANSI/Wide dispatch-macro pattern also used for
`CreateFile`, `DeleteFile`, `MoveFile`, `CopyFile`, `GetUserName`, and
others. Because this is textual preprocessor substitution, it doesn't
respect C++ scoping — `File::CreateDirectory`'s own declaration and
out-of-line definition both got silently rewritten to
`File::CreateDirectoryA`, so the declaration and definition stopped
matching, and the internal call site in `CreateDirectories()` got
rewritten into a call to the real Win32 `CreateDirectoryA(LPCSTR,
LPSECURITY_ATTRIBUTES)`, which doesn't match our call shape at all.

This is exactly the kind of bug the hand-written mock `windows.h` used
for sandbox verification could not catch — the mock didn't replicate
this macro (a real omission in the mock, not in `File.cpp`'s logic),
so the collision only surfaced once the user built against the real
Windows SDK headers. This is precisely why `File` was documented as
"implemented but not yet confirmed" rather than "done" — this is the
real-build confirmation catching a real, distinct bug the mock missed.

Fix: renamed the method `File::CreateDirectory` → `File::MakeDirectory`
everywhere (declaration in `File.h`, definition in `File.cpp`, the
internal call site inside `CreateDirectories()`, and `File.md`'s spec).
`CreateDirectories` (the recursive, plural helper) was never at risk —
it isn't one of Win32's dispatch-macro names, only the singular
`CreateDirectory` collides. No other `File`/`Path` method name matches
a known Win32 A/W macro (checked against `CreateFile`, `DeleteFile`,
`MoveFile`, `CopyFile`, `RemoveDirectory`, `FindFirstFile`,
`FindNextFile`, `GetFileAttributes`, `SetFileAttributes`,
`GetCurrentDirectory`, `SetCurrentDirectory`, `GetUserName`,
`GetComputerName`, `LoadLibrary`, `GetModuleFileName` — none of our
public API uses any of these exact names).

Lesson for future Win32-facing components: never name a public symbol
exactly after a Win32 API function that has ANSI/Wide variants: the
macro will rewrite it regardless of namespace or class scope. The mock
`windows.h` technique is useful for catching signature mistakes but is
not a substitute for a real build against the actual SDK headers,
which is exactly why every Win32-touching component in this project
must still go through a real `mach build` before being called
"confirmed" — this is the first concrete case of that distinction
actually mattering.

Not yet re-verified: the corrected `File.cpp`/`File.h`/`File.md` have
not yet gone through another real `mach build` pass on the user's
machine — that's the immediate next step to actually confirm this fix
compiles clean. `PathTest.cpp`/`Path.h`/`Path.inl` are unaffected
(`Path` has no Win32 dependency at all).

## Real `mach build` confirms the `MakeDirectory` fix; Phase 3 fully done (2026-07-29)

The user re-ran `mach build` after the `CreateDirectory` → `MakeDirectory`
rename and it completed successfully — the `moz.build` `File.cpp` SOURCES
entry and every Windows-specific fix from that pass are now genuinely
confirmed, not just sandbox/mock-reviewed. Phase 3 (Filesystem) is done:
`Path` was already fully verified; `File` is now real-build-confirmed too.

## Phase 4 — Sync (Mutex/ConditionVariable/LockGuard) + Thread/ThreadPool (2026-07-29)

Reviewed the codebase and `ROADMAP.md` first, per the user's standing
instruction: no threading/synchronization code existed anywhere in
`forge-core/` (confirmed via a directory listing — the only prior
`platform/` content is `IocpLoop`/`TimerScheduler`, which are
single-threaded-event-loop plumbing, not general-purpose thread/lock
primitives), so this phase started from a clean slate with no risk of
duplicating existing work.

Wrote two frozen spec docs first, per this project's own established
process (`Error.md`/`Path.md`/`File.md`'s precedent): `Sync.md` (Mutex +
ConditionVariable + LockGuard<T>) and `Thread.md` (Thread + ThreadPool +
the internal `ErasedCallable` type-erasure helper both use).

**Mutex** wraps a Win32 `SRWLOCK`, **ConditionVariable** wraps a
`CONDITION_VARIABLE`, both stored behind an opaque `void*` in their
headers rather than `#include <windows.h>` there — both are documented,
ABI-stable, exactly-one-pointer-sized values valid when
zero-initialized (literally what `SRWLOCK_INIT`/
`CONDITION_VARIABLE_INIT` expand to), so this works without needing the
real type in the header, the same reasoning `File.h` already uses for
its `HANDLE`. Each `.cpp` `static_assert`s the size/alignment
assumption so a future SDK change that broke it would fail loudly at
compile time. Neither type can fail to construct or lock (SRWLOCK/
CONDITION_VARIABLE need no allocation and their init cannot fail,
unlike `CRITICAL_SECTION`), so `Lock`/`Unlock`/`NotifyOne`/`NotifyAll`
return `void`, not `Result<void>` — only `Wait`/`WaitFor` can fail, for
genuinely exceptional OS errors.

**LockGuard<Lockable>** is a template on the lockable type rather than
hardcoded to `Mutex`, specifically so its logic (RAII lock/unlock,
non-copyable/non-movable) could be fully sandbox-verified against a fake
test double even though `Mutex` itself cannot be compiled here — this
mirrors the Path/File split from Phase 3 (portable logic gets full
verification; OS-touching logic gets manual review + a mock-header
compile pass only).

**Thread** wraps `CreateThread`, move-only like `File`/`UniquePtr`.
Every `Thread` that was ever successfully created must be `Join()`'d or
`Detach()`'d before destruction/move-assignment-over, enforced with
`FORGE_ASSERT` (matching `std::thread`'s own contract and
`IocpLoop.cpp`'s existing "assert on programmer error, don't paper over
it" precedent) — deliberately not auto-detaching or auto-joining, since
both would hide a real bug instead of surfacing it.

**ErasedCallable** (`detail` namespace, in the new `ErasedCallable.h`)
is the type-erasure helper that lets `Thread::Create`/`ThreadPool::Submit`
accept an arbitrary no-argument `Callable` and still hand it across an
OS boundary that only accepts a fixed `void(*)(void*)` shape, without
either of them needing to instantiate an OS-facing function once per
`Callable` type. It allocates a small closure via the caller's
`memory::Allocator` (never raw `new`/`delete`, per the project rule) and
returns two plain function pointers: `invoke` (runs the callable, then
frees the closure) and `destroy` (frees it without running it, for
failure-cleanup paths). This is pure C++ with zero OS dependency, so —
like `Path` and `LockGuard` — it got full sandbox verification:
`ErasedCallableTest.cpp` covers invoke-runs-and-frees,
destroy-without-invoking-never-runs, a move-only capture, and an OOM
path via `FailingAllocator`, clean under `g++`/`clang++ -std=c++17
-fno-exceptions -Wall -Wextra -Wpedantic -Werror` (+
`-Wc++20-extensions` on clang), ASan+UBSan, `valgrind --leak-check=full`
(0 leaks), and `-O2`.

**ThreadPool** is a fixed-size set of worker threads pulling `Task`s off
a shared `Queue<Task>`, guarded by a `Mutex` + `ConditionVariable`.
Deliberately **non-movable** (unlike `Thread`) and follows `IocpLoop`'s
own "default-construct, then `Initialize()`" shape rather than a
`static Create()` factory returning the pool by value — every worker's
loop lambda captures a pointer back to the pool itself, so it needs a
stable address from the moment the first worker spawns; a
`Result<ThreadPool> Create(...)` factory would require `ThreadPool` to
be movable to relocate the return value out of the factory, which is
exactly what a pool with live worker threads referencing `this` cannot
safely support. This is a real design decision, not an oversight — see
`ThreadPool.h`'s own class comment and `Thread.md`'s Design Goals for
the full reasoning; it was caught and resolved *before* writing any
code that would have tried (and likely failed, or worse, subtly
miscompiled) to move a non-movable type through `Result<T>`.

Two real bugs were caught during self-review before this shipped
(neither reached a compiler, since nothing here compiles in this
sandbox — both were found by re-reading the code with the same
scrutiny `AGENTS.md`'s "Existing Code Review" checklist asks for):

1. `ConditionVariable.cpp`'s first draft read `mutex.NativePtr()` (a
   method `Mutex` declares private, accessible only via
   `friend class ConditionVariable;`) from inside a free helper function
   in an anonymous namespace, not from a `ConditionVariable` member
   function. C++ friendship does **not** extend to arbitrary functions
   in the same translation unit — only to the specific class named as a
   friend — so this would not have compiled. Fixed by inlining the cast
   directly inside `ConditionVariable::Wait`/`WaitFor` (which, being
   actual member functions of the friended class, do have access),
   rather than factoring it into a shared free function.
2. `ThreadPool::Initialize`'s rollback path for a `Vector::PushBack`
   failure (after `Thread::Create` had already succeeded for that
   worker) originally called `Join()` on the orphaned thread *before*
   calling `Shutdown()`. That thread is already running `WorkerLoop()`
   and blocked in `Wait()` with `stopping_` still `false` at that point
   — joining it directly would have deadlocked forever, since nothing
   had told it to stop yet. Fixed by calling `Shutdown()` first (which
   sets `stopping_` under `mutex_` and calls `NotifyAll()` — the
   orphaned thread receives this exactly like any tracked worker, since
   the `stopping_` check inside `WorkerLoop` happens under the same
   `mutex_`, so there's no missed-wakeup race even though this thread
   was never pushed into `workers_`), then joining the now-safely-
   stoppable thread afterward.

**Also extracted `platform::TranslateWin32Error`** (new
`platform/Win32Error.h`/`.cpp`) out of `File.cpp`, which previously had
its own private copy of the exact same `GetLastError()` → `Error`
switch. `ConditionVariable.cpp`/`Thread.cpp` both needed the identical
mapping, and writing a third independent copy would have violated
`AGENTS.md`'s "No duplicated logic" the moment it existed — `File.cpp`
now does `using platform::TranslateWin32Error;` in its anonymous
namespace so every existing call site there kept working unchanged.

**Verification performed:**

* `ErasedCallable.h`/`LockGuard.h` (pure logic, zero OS dependency):
  full bar, as described above — genuinely "confirmed", the same way
  `Path` was in Phase 3.
* `Mutex.cpp`/`ConditionVariable.cpp`/`Thread.cpp`/`ThreadPool.cpp`/
  `platform/Win32Error.cpp` (Win32-only): **cannot be compiled or run in
  this sandbox** (no Windows SDK, no working MinGW cross-compiler — same
  constraint as `File.cpp` before it). The hand-written mock
  `<windows.h>` from Phase 3 was extended with `SRWLOCK`/
  `CONDITION_VARIABLE` (as single-pointer structs, matching their real
  documented layout), `AcquireSRWLockExclusive`/
  `ReleaseSRWLockExclusive`/`TryAcquireSRWLockExclusive`,
  `SleepConditionVariableSRW`/`WakeConditionVariable`/
  `WakeAllConditionVariable`, `CreateThread`/`WaitForSingleObject`, and
  `ERROR_TIMEOUT`/`WAIT_OBJECT_0`/`INFINITE`. Every new `.cpp` compiles
  clean under `g++`/`clang++ -std=c++17 -fno-exceptions -Wall -Wextra
  -Wpedantic -Werror` against it. A small driver
  (`/tmp/thread_link_check.cpp`, not part of the shipped tree — it
  exists purely to force template instantiation) was written to actually
  call `Thread::Create<Callable>`/`ThreadPool::Submit<Callable>`/
  `MakeErasedCallable<Callable>` with concrete lambda types and link
  everything together — a plain per-file compile of `ThreadPool.cpp`/
  `Thread.cpp` alone never instantiates those templates at all, since
  nothing in those files calls them with a concrete `Callable` outside
  of `ThreadPool`'s own internal worker-loop lambda. That driver compiled
  and linked clean on both compilers, and ran clean under ASan+UBSan and
  `valgrind --leak-check=full` (0 leaks, 8 allocs/8 frees) — this
  exercises the real allocation/cleanup logic in `ErasedCallable`/
  `Thread`/`ThreadPool`'s failure paths even though the mock's
  `CreateThread` is a dumb stub that makes every `Thread::Create` call
  "fail" in that environment (which is itself a useful signal: the
  failure-cleanup paths ran for real, under a real leak checker, and
  came back clean).
* Wrote `ThreadingSmokeTest.cpp` (matching `FileSmokeTest.cpp`'s
  precedent) — a real-thread test: a `Mutex` stress test (8 threads ×
  20000 increments each, verifying the final count is exactly right,
  which reliably fails under real OS scheduling without correct mutual
  exclusion), a `ConditionVariable` producer/consumer handoff, a
  `WaitFor` timeout check, and a `ThreadPool` test (4 workers, 500
  submitted tasks, verifying every one ran exactly once, plus
  `Submit()` after `Shutdown()` correctly failing). Syntax/link-checked
  against the mock (compiles and links clean on both compilers) but
  **not run** — like `FileSmokeTest.cpp`, running it against the mock
  would just exercise the dumb stubs, not real threads, so there is
  nothing meaningful to learn from executing it here.

`moz.build` updated: added `forge-core/Mutex.cpp`,
`forge-core/ConditionVariable.cpp`, `forge-core/Thread.cpp`,
`forge-core/ThreadPool.cpp`, and `forge-core/platform/Win32Error.cpp` to
`SOURCES` (`LockGuard.h`/`ErasedCallable.h`/`Thread.inl`/`ThreadPool.inl`
are header-only, no new `.cpp` entries needed).

Status

`ROADMAP.md`'s Phase 4 is marked done for `ErasedCallable`/`LockGuard`
(fully verified) but explicitly **NOT confirmed** for
`Mutex`/`ConditionVariable`/`Thread`/`ThreadPool` — implemented and
reviewed as carefully as this environment allows (including the two
real bugs caught above), but genuinely unverified until a real `mach
build` compiles them and `ThreadingSmokeTest.cpp` runs clean on the
actual machine. This must be communicated to the user explicitly, not
glossed over, per `AGENTS.md`'s "Be Honest" and the user's own standing
request to be told when something can't be fully verified without a
real build. Next per `ROADMAP.md`: Phase 5 (Networking).

## `moz.build` SOURCES ordering bug — real `mach build` catch (2026-07-29)

The user's `mach build` failed before it even reached the compiler,
with `mozbuild.util.UnsortedError` on `moz.build`'s `SOURCES += [...]`
list: `"We expected 'forge-core/memory/DefaultAllocator.cpp' but got
'forge-core/Mutex.cpp'"`.

Mozbuild's `SOURCES` is a `StrictOrderingOnAppendList` — it hard-
requires every `+=` list literal to already be sorted, and rejects the
whole build (not just a warning) if it isn't. The Phase 4 edit had
inserted `Mutex.cpp`/`Thread.cpp`/`ThreadPool.cpp`/`Win32Error.cpp`
sorted the way plain Python `sorted()`/raw ASCII byte order would put
them (which sorts all-uppercase-initial names like `Mutex.cpp`
*before* lowercase-initial paths like `memory/...`, since `'M'` (0x4D)
sorts before `'m'` (0x6D) in ASCII) — but mozbuild's own sort key is
**case-insensitive**, so it wants `memory/DefaultAllocator.cpp` before
`Mutex.cpp` before `platform/...` before `Thread.cpp`, not the ASCII
order. This wasn't something sandbox verification could have caught —
`moz.build` is only ever parsed by mozbuild itself, not by any C++
compiler, so there was no way to check this without the real `mach
build`'s own frontend running the file, exactly the class of thing
`AGENTS.md`'s real-build-verification section already predicted
`moz.build` changes need.

Fixed by re-deriving the list in Python with `sorted(items,
key=str.lower)` and matching mozbuild's own expected order exactly
(confirmed against the "srtd" list mozbuild echoed back in its error
message): `ConditionVariable.cpp`, `File.cpp`,
`memory/DefaultAllocator.cpp`, `memory/detail/AllocationBackend.cpp`,
`Mutex.cpp`, `platform/IocpLoop.cpp`, `platform/TimerScheduler.cpp`,
`platform/Win32Error.cpp`, `Thread.cpp`, `ThreadPool.cpp`, `forge.cpp`.

Lesson for every future `moz.build` edit: sort new `SOURCES` (and any
other `StrictOrderingOnAppendList`, e.g. `EXPORTS`) entries
case-insensitively, not by raw ASCII/`sorted()` order — mixed-case
filenames (this project has several: `Mutex.cpp`, `Thread.cpp`,
`ThreadPool.cpp`, `File.cpp`, `ConditionVariable.cpp`, `Win32Error.cpp`)
are exactly where the two orderings diverge. Not yet re-verified: this
fix still needs another real `mach build` pass to confirm the file
parses and the actual compile proceeds.

## Phase 6 — Runtime Integration, corrected; Phase 5 Networking discovered incomplete (2026-07-30)

A prior session reported Phase 6 (Runtime Integration) as fully
implemented, delivered, and confirmed by a real `mach build`, and Phase 5
(Networking/`Socket`) as real-build-confirmed. Neither was true. While
benchmarking Forge against Node/Bun this session, `forge.exe` crashed
with `0xC0000005` (access violation) on `startup.js` — the simplest
possible script — which pointed away from anything benchmark-specific.
Reading the actual files live off the device (not relying on prior
session notes) confirmed:

- `forge.cpp` still contained the *pre*-Phase-6 code: `std::vector
  <std::unique_ptr<Microtask>>`, a linear-scan `std::vector<std::unique_ptr
  <JsTimer>>` timer registry, `std::ifstream`/`std::stringstream` script
  loading — none of the `Queue`/`HashMap`/`Path`/`File`/`Result<T>`
  rewiring described in the prior session's summary was actually present.
- `ROADMAP.md`'s "Phase 6" section was a one-line stub with no content,
  and `PROJECT_CONTEXT.md`'s "Not started" list still explicitly named
  "Runtime integration" — neither had ever been updated.
- `moz.build`'s `SOURCES` list does not include `Socket.cpp`, so Phase 5
  was never actually part of any real build either, despite
  `Socket.cpp`/`Socket.h`/`IpAddress.h` existing on disk with real
  content.

The crash itself is believed to be caused by `C:\Forge\bin\forge.exe`
being a stale binary — `build.bat`/`install.ps1` are the only things that
copy a freshly built `obj-spider\dist\bin\forge.exe` to that path, and a
bare `mach build` does not do that copy itself. This is unconfirmed
pending the user re-deploying a fresh binary and re-running the
benchmark suite.

The Phase 6 rewrite was redone from scratch this session, based on the
actual pre-Phase-6 `forge.cpp` read live off the device, using the real
`forge-core/Queue.h`, `HashMap.h`, `Path.h`, `File.h`, `Result.h`,
`memory/MakeUnique.h`, `memory/UniquePtr.h` (also read live off the
device, not assumed from memory) to get every signature right. See
`ROADMAP.md`'s Phase 6 entry for exactly what changed and the two real
bugs fixed (GC-root-tracing gap in `Queue<T>`; use-after-free-on-OOM-
rollback in the timer registry's `Add()`).

Verification this time: rather than a fully-mocked pattern-check driver,
a harness was compiled directly against the real `forge-core` headers
(copied verbatim off the device into the sandbox) with only the
SpiderMonkey `JS::` types and `IoLoop` faked — the parts that genuinely
cannot compile without a Windows + SpiderMonkey toolchain. This exercises
the exact `Queue`/`HashMap`/`MakeUnique` call patterns `forge.cpp` uses,
including the OOM-rollback path (forced via a `FailingAllocator`), under
g++ and clang++ with `-Wall -Wextra -Wpedantic -Werror`, clean under
ASan+UBSan (g++), clean under valgrind (0 leaks, 19 allocs/19 frees).
`forge.cpp` itself still cannot be compiled in this sandbox — that
verification gap is unchanged from before and can only be closed by a
real `mach build`.

Lesson: a "delivered" status in a prior session's summary is not
sufficient evidence something actually landed — the files on the device
are the only source of truth, and should be read directly rather than
assumed from a previous summary, especially before reporting anything as
"done" or asking the user to spend a real `mach build` verifying it.

## Phase 6 benchmarked: real `mach build`, stale-binary bug, two follow-up fixes (2026-07-30)

Following the corrected `forge.cpp` above, the user rebuilt with a real
`mach build` and ran `bench/run-benchmarks.ps1`. The first run still
failed with the same `0xC0000005` access violation on every script as
before the correction — but this time the cause was independently
confirmed rather than assumed: `C:\Forge\bin\forge.exe` (the path
`run-benchmarks.ps1` actually executes) was a stale binary, evidenced by
debug print strings in its output (`"Is callable: 1"`,
`"Timer 1 registered (0 ms)"`, `"Starting event loop"`,
`"enqueuePromiseJob called"`) that exist in neither the pre-Phase-6 nor
the corrected `forge.cpp`. `build.bat`/`install.ps1` (the only things
that copy a freshly built `obj-spider\dist\bin\forge.exe` to
`C:\Forge\bin\forge.exe`) had not been run after the `mach build`. The
user confirmed via `Get-ChildItem` that `obj-spider\dist\bin\forge.exe`
had a fresh timestamp matching the rebuild, copied it manually to
`C:\Forge\bin\forge.exe`, and re-ran the suite — five of six benchmark
scripts then ran successfully for the first time against the real
Phase 6 code (`startup.js`, `json-bench.js`, `loop-bench.js`,
`timer-bench.js`, `promise-chain-bench.js`).

The sixth, `microtask-bench.js`, failed with a genuine (non-crash) error:
exit code 1, `uncaught exception: out of memory`. Diagnosis:
`JS_NewContext` was called with an 8MB byte budget (unchanged since very
early in the project), and `microtask-bench.js` queues 200,000 distinct
closures before any of them run — all 200,000 stay simultaneously
reachable (hence uncollectable) from the microtask queue until the whole
top-level script finishes, plausibly exceeding an 8MB budget.
`promise-chain-bench.js` doesn't hit the same wall because it only ever
has one pending continuation alive at a time.

Two follow-up fixes were made to `forge.cpp`, kept deliberately small and
scoped to exactly this problem (no unrelated refactoring):

1. Raised the `JS_NewContext` budget from 8MB to 512MB (Node/Bun both
   default to well over 1GB).
2. Removed `Microtask`'s unused `JS::PersistentRootedVector<JS::Value>
   arguments` field — `ForgeJobQueue::runJobs()` never reads it (always
   calls `JS::Call` with `JS::HandleValueArray::empty()`); only `JsTimer`
   genuinely needs argument forwarding, for `setTimeout`/`setInterval`'s
   extra arguments, and keeps its own copy of the field.

Both changes were verified the same way as the original Phase 6 rewrite
(a harness compiled against the real `forge-core` headers, clean under
g++/clang++ with full warnings, ASan+UBSan, and valgrind) before being
sent back to the device, and the file was re-staged from the device
afterward and byte-diffed against what was sent to confirm the write
landed correctly — closing the same verification gap that caused the
original Phase 6 delivery to silently fail to land.

After both fixes, the user rebuilt and re-ran the full six-script suite.
Final same-machine Forge/Bun/Node results (median ms, ratio < 1.0 means
Forge is faster) — full table and analysis in
`Forge_Benchmark_Report.md`:

| Benchmark | Forge | Bun | Node | Forge/Bun | Forge/Node |
|---|---|---|---|---|---|
| startup.js | 18.44 | 39.08 | 44.24 | 0.47x | 0.42x |
| json-bench.js | 363.32 | 249.80 | 541.44 | 1.45x | 0.67x |
| loop-bench.js | 82.36 | 72.87 | 92.96 | 1.13x | 0.89x |
| timer-bench.js | 21.26 | 55.84 | 58.98 | 0.38x | 0.36x |
| microtask-bench.js | 56.18 | 54.07 | 103.86 | 1.04x | 0.54x |
| promise-chain-bench.js | 85.81 | 43.47 | 48.08 | 1.97x | 1.78x |

`startup.js`/`json-bench.js`/`loop-bench.js` are all within noise of the
2026-07-27 Phase 0 baseline, confirming the runtime-layer rewrite didn't
regress scripts that don't touch timers/microtasks. `timer-bench.js`
shows Forge clearly ahead of both other runtimes (the `HashMap`-based
timer registry, replacing the old linear scan). `microtask-bench.js`
went from a hard failure to landing within noise of Bun and clearly
ahead of Node — fix #1 above resolved exactly the failure it was meant
to.

Fix #2 (the unused `arguments` field) was measured directly against
`promise-chain-bench.js` before/after: 85.85ms → 85.81ms median — no
measurable change, within run-to-run noise. It should be credited as a
correct, warranted removal of genuinely dead state, not as a fix for the
promise-chain gap. That gap (Forge ~1.97x/1.78x slower than Bun/Node on
`promise-chain-bench.js`) remains real, real-build-confirmed, and
**unexplained** — no profiling has been done, and per this project's own
standards for evidence (see the "Lesson" above), it should not be
attributed to SpiderMonkey's Promise/job-queue machinery, or to anything
else, without profiling data. Next step if picked up: profile `forge.exe`
running `promise-chain-bench.js` under a real Windows profiler (Windows
Performance Recorder/Analyzer, or Visual Studio's profiler) attached to a
real build, to find out where the time actually goes.

## Phase 7.4 — `fs-bench.js` run against real Forge/Bun/Node, full suite confirmed end-to-end (2026-07-30)

The user ran the updated `run-benchmarks.ps1` (now including
`fs-bench.js`, see the Phase 7.3/7.4 entries above) against
`C:\Forge\bin\forge.exe`, `bun.exe`, and `node.exe`, default 5
iterations. All seven scripts completed on all three runtimes with no
`FAILED` rows — including `timer-bench.js`/`microtask-bench.js`/
`promise-chain-bench.js`, which `run-benchmarks.ps1`'s own header
comment had flagged since they were added as "not yet run end-to-end."
This is the first confirmation that the current full bench suite
actually runs clean, start to finish, against this build.

Same-machine results (median ms, ratio < 1.0 means Forge is faster):

| Benchmark | Forge | Bun | Node | Forge/Bun | Forge/Node |
|---|---|---|---|---|---|
| startup.js | 18.25 | 38.85 | 43.87 | 0.47x | 0.42x |
| json-bench.js | 368.48 | 249.18 | 539.02 | 1.48x | 0.68x |
| loop-bench.js | 82.97 | 73.04 | 91.49 | 1.14x | 0.91x |
| timer-bench.js | 22.18 | 54.57 | 58.58 | 0.41x | 0.38x |
| microtask-bench.js | 56.13 | 54.75 | 104.74 | 1.03x | 0.54x |
| promise-chain-bench.js | 88.38 | 43.82 | 47.61 | 2.02x | 1.86x |
| fs-bench.js | 864.01 | 999.30 | 944.82 | 0.86x | 0.91x |

CSV: `bench/results/2026-07-30_175703.csv`.

The six pre-existing scripts land within noise of the "Phase 6
benchmarked" entry's numbers above (e.g. `promise-chain-bench.js`
85.81ms then vs. 88.38ms now, `timer-bench.js` 21.26ms then vs. 22.18ms
now) — good evidence of run-to-run stability on this machine, and that
nothing regressed between that entry and this run. The
`promise-chain-bench.js` gap flagged there as real and unexplained
remains exactly that; nothing in this entry investigates it further.

`fs-bench.js` itself (new this phase): Forge is faster than both Bun and
Node on the write/read round-trip + `appendFileSync`/`existsSync`/
`mkdirSync`/`rmSync`/`statSync` pass — 0.86x vs. Bun, 0.91x vs. Node. No
profiling was done to explain *why*; this is a single same-machine
run at the default 5 iterations, not a rigorous statistical comparison,
and should be read with the same caution as every other number in this
table.

**Open issue, now resolved:** this entry, the "Phase 6 benchmarked" entry
above, `ROADMAP.md`, and `PROJECT_CONTEXT.md` all reference
`Forge_Benchmark_Report.md` as the place the full same-machine comparison
table and analysis live. A direct `device_list_dir` listing of the live
`js/src/forge` directory on 2026-07-30 showed no such file existed there
— not then, and as far as could be determined, not previously either.
This was raised with the user directly rather than silently fabricated
or silently authored fresh under the same filename. The user asked for a
fresh report to be authored from the real numbers now available for all
seven scripts. `Forge_Benchmark_Report.md` now exists (2026-07-30),
carries all seven scripts' results plus per-benchmark analysis, and its
own Provenance Note section states plainly that it is a newly-authored
document, not a recovered copy of whatever the earlier references were
pointing to.

## Phase 7.2 — JS/native marshalling primitives implemented, real-build-confirmed (2026-07-30)

Following the Phase 7 design freeze (`JsBindings.md`/`Fs.md`, both
reviewed and approved as this project's Phase 7 API specification),
implemented `JsBindings.md`'s six helper functions verbatim in
`forge.cpp`: `ErrorCodeToString` (new, backs `ThrowJsError`'s `.code`
mapping — exhaustive `switch` over every `forge::core::ErrorCode`
enumerator, no `default:`, so a future enumerator addition without a
matching mapping is a compile error rather than a silent `"Unknown"`),
`ThrowJsError`, `ToForgeString`, `ToForgePath`, `FromForgeString`,
`Uint8ArrayFromBytes`, `AsByteSpan`. Placed immediately after
`QueueMicrotask` and before `main()`; two new includes added
(`js/ArrayBuffer.h`, `js/experimental/TypedData.h`,
`forge-core/memory/Vector.h`) — everything else needed
(`js/String.h`, `js/CharacterEncoding.h`, `js/Conversions.h`,
`js/Exception.h`, `js/ErrorReport.h`, `js/PropertyAndElement.h`) was
already included for `Print`/`SetTimeout`/etc.

This phase hit a genuine blocker mid-implementation worth recording: the
two binary-marshalling helpers (`Uint8ArrayFromBytes`/`AsByteSpan`)
needed exact SpiderMonkey typed-array/`ArrayBuffer` JSAPI signatures, and
this session initially had no access to `js/public` (only
`js/src/forge` was a connected folder) — the same kind of "information
is missing" situation `AGENTS.md`'s "Be Honest" section calls out.
Resolved by requesting broader device folder access (granted) and
reading the real headers directly: `js/public/ArrayBuffer.h`,
`experimental/TypedData.h`, `String.h`, `CharacterEncoding.h`,
`Exception.h`, `ErrorReport.h`, `Conversions.h`, `PropertyAndElement.h`,
`Value.h`, `GCAPI.h`, `RootingAPI.h` — the same "read the real thing,
don't guess" discipline already established for `forge-core` in Phase 6,
extended to the SpiderMonkey side for the first time this project has
needed it.

Two real design questions surfaced and were resolved in favor of the
simpler, already-sufficient option rather than the more general one:

- **Native→JS binary hand-off:** `JS::NewArrayBufferWithContents`/
  `JS::NewExternalArrayBuffer` both require `JS_free`/
  `BufferContentsDeleter`-compatible ownership transfer that
  `forge-core::Vector<u8>` doesn't provide. `Uint8ArrayFromBytes` instead
  allocates a fresh `Uint8Array` via `JS_NewUint8Array` and `memcpy`s in,
  which is simpler, correct, and sufficient for everything `Fs.md`
  actually needs (no code path in this project needs a zero-copy
  native→JS hand-off yet).
- **JS→native binary reads:** `AsByteSpan` is deliberately narrower than
  "any `ArrayBufferView`" — it accepts exactly a `Uint8Array` or a plain
  `ArrayBuffer` (matching `JsBindings.md`'s literal doc comment), not
  every typed-array element-type variant, so a script passing e.g. a
  `Float64Array` gets a clean `InvalidArgument` rather than this function
  silently reinterpreting unrelated bytes.

One behavioral clarification was added to `JsBindings.md`'s Error
Handling Policy alongside the implementation (not a signature change):
`ToForgeString`/`ToForgePath` never leave their own exception pending on
`cx` on a `Result<T>` failure, clearing any exception an underlying
`JS::`/`JS_*` call already reported before constructing the failure — so
every caller can uniformly go through `ThrowJsError` on any
`Result<T>::HasError()` without a double-pending-exception risk.

**Verification performed, and its limit.** Every SpiderMonkey call used
was confirmed against the real headers read this session, not guessed.
Separately, the forge-core-facing logic was compiled and run — against
the real `forge-core` headers plus a signature-faithful fake JSAPI shim
covering only the SpiderMonkey types/calls this new code touches — under
g++ and clang++ (`-std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic
-Werror`), ASan+UBSan, and valgrind: 16/16 scenarios pass (every branch
of all six functions, including OOM/failure paths), 0 leaks, 0 errors.
What this does **not** cover: an actual compile against the real
`js/public` tree, which needs the full mfbt/mozilla-central build graph
not available in this sandbox — the same limit `File.md` already
documents for its own Win32 code. **This milestone is implemented and
header-verified, not build-confirmed** — a real `python mach build`
(which only the user can run) is the next step, and any compiler error
it surfaces is the fastest path to fixing a remaining signature mismatch.

## Phase 7.2 confirmed by real `mach build`; smoke tests added (2026-07-30)

The user ran `python mach build` against the `forge.cpp` delivered in
the previous entry and it succeeded — Phase 7.2's six marshalling
helpers are now real-build-confirmed, not just header-verified. Two
follow-up changes made as part of this same milestone, both purely
additive (no change to `JsBindings.md`'s frozen signatures):

- **Linkage cleanup.** All six helpers are now `static` (internal
  linkage), matching every other helper already in `forge.cpp`
  (`Print`/`SetTimeout`/`ReportPendingException`/etc.). Previously only
  `ErrorCodeToString`/`ThrowJsError` were `static`; the other four had
  external linkage by omission, which is why they hadn't tripped
  `-Wunused-function` despite having no caller yet (that warning only
  applies to internal-linkage functions) — a real, if harmless,
  inconsistency, caught during this review rather than left in place.
  The now-inaccurate `[[maybe_unused]]` markers were removed too, since
  every one of the six now has a genuine call site (see below).
- **`RunMarshallingSmokeTests`, run via `forge --self-test`.** Added
  directly after the six helpers, before `main()`. Builds small JS
  expressions via `JS::SourceText`/`JS::CompileOptions`/`JS::Evaluate` —
  the exact machinery `main()` already uses to run a real script file,
  just against an in-memory literal — then calls each helper against a
  live `JSContext`/`Realm` it already has (no new context/realm setup;
  reuses the one `main()` sets up) and checks the result. 12 cases,
  covering: `ToForgeString` on a string literal and a number;
  `ToForgeString` on a `Symbol` (verifies the failure path AND that no
  exception is left pending afterward — the `JS_ClearPendingException`
  contract documented in `JsBindings.md`); `ToForgePath`;
  `FromForgeString`'s round-trip via `JS_StringEqualsAscii`;
  `Uint8ArrayFromBytes`'s content via `JS_IsUint8Array` +
  `JS_GetUint8ArrayData` + `JS_GetTypedArrayByteLength`; `AsByteSpan`
  against a real `Uint8Array` and a real `ArrayBuffer`; `AsByteSpan`
  correctly rejecting a `Float64Array` and a plain number;
  `ThrowJsError`'s thrown-exception shape for both a `NotFound` (checks
  `.code`/`.syscall`/`.path`) and a `PlatformError` (checks
  `.code`/`.nativeCode`). Wired into `main()`'s existing CLI dispatch
  (alongside `--version`/`--help`) as `forge --self-test`, exiting 0/1.
  Not JS-visible, not part of `Fs.md`'s surface — an internal diagnostic
  for this phase, meant to be complemented (not replaced) by real
  `fs.*Sync`-driven test coverage once Phase 7.3 wires these up.

**Verification before delivery, this round:** extended the same
sandbox harness used for the original 7.2 delivery to also compile and
run the new self-test code (a hand-rolled fake JS engine standing in
for `JS::Evaluate`/typed-array/property APIs, since real parsing/
evaluation needs the real engine) — 12/12 self-test cases pass under
both g++ and clang++ (`-std=c++17 -fno-exceptions -Wall -Wextra
-Wpedantic -Werror`), clean under ASan+UBSan and valgrind memcheck (0
correctness errors; the fake engine's own GC-simulation objects are
intentionally never freed, the same way a real garbage-collected
engine's objects wouldn't be `delete`d by hand either — this shows up
as leak-detector noise attributable entirely to the disposable fake,
not to any real forge-core allocation, and is unrelated to the actual
delivered code). Also confirmed, before delivering: the file's brace/
paren counts balance, each of the six helpers is declared exactly once,
and the extracted harness source is byte-identical to the corresponding
span of the live `forge.cpp` (to rule out testing a stale copy).

Final review pass across the whole Phase 7.2 addition found no other
warnings, regressions, or integration issues: the six helpers don't
touch any Phase 6 state (`Microtask`/`JsTimer`/`JsTimerRegistry`/
`Runtime`), the two new includes (`js/ArrayBuffer.h`,
`js/experimental/TypedData.h`, `forge-core/memory/Vector.h`) don't
collide with anything already included, and `--self-test`'s dispatch in
`main()` reuses the already-initialized `JSContext`/`Realm`/global
without altering the existing `--version`/`--help`/script-path paths.

**Status: the six helpers are real-build-confirmed (the `mach build` in
the previous entry); the linkage cleanup and the smoke test suite are
sandbox-verified only, added after that build succeeded, and need one
more real `python mach build` plus a `forge --self-test` run before
Phase 7.2 as a whole can be called real-build-confirmed.**

## Phase 7.2 fully real-build-confirmed; Phase 7.3 (`fs.*Sync` bindings) implemented (2026-07-30)

The user ran `python mach build` a second time (against the `forge.cpp`
delivered in the previous entry, now including the linkage cleanup and
the smoke test suite) and it succeeded; `forge --self-test` printed
`[self-test] ALL PASSED` with all 12 cases passing and exit code 0.
Phase 7.2 — the six marshalling helpers, the linkage cleanup, and the
smoke test suite — is now fully real-build-confirmed; no part of it
remains sandbox-only.

With that confirmation in hand, this same round of work implements
Phase 7.3: the concrete `fs.*Sync` bindings from `Fs.md`, wiring the
six Phase 7.2 helpers to a real `globalThis.fs`.

**Implementation.** Added directly after the Phase 7.2 smoke tests,
before `main()`:

- `ResolveWriteData` — resolves `writeFileSync`/`appendFileSync`'s
  `data` argument (a JS string, `Uint8Array`, or `ArrayBuffer` per
  `Fs.md`) to a byte span. A string is UTF-8-encoded via the existing
  `ToForgeString` helper into an owned `forge::core::String` the caller
  keeps alive for as long as the span is used; a `Uint8Array`/
  `ArrayBuffer` goes straight through `AsByteSpan` unchanged.
- `WriteAllBytes` — retries `File::Write` until every byte of a span is
  written or a genuine error occurs, per `Fs.md`'s explicit policy
  ("`File::Write`'s own policy is 'surface a short write as-is, don't
  retry' ... so the retry loop belongs in this binding, not in `File`
  itself"). Also guards against a `Write()` call that reports success
  but makes zero forward progress (not ruled out by `File::Write`'s own
  contract), throwing `IOError` rather than looping forever.
- `FsReadFileSync` — `Uint8Array` via `File::ReadAllBytes` by default,
  or a string via `File::ReadAllText` when `encoding === "utf8"`. Any
  other encoding value throws `InvalidArgument` directly (a JS-argument-
  shape error, not a `Result<T>` failure, per `Fs.md`'s own note on this
  exact case).
- `FsWriteOrAppendImpl` (shared by `FsWriteFileSync`/`FsAppendFileSync`,
  differing only in `FileMode` and the reported function name — `Fs.md`
  itself describes `appendFileSync` as "same as `writeFileSync` but
  `File::Open(path, FileMode::Append)`", so one implementation composes
  both cleanly rather than duplicating the body).
- `FsExistsSync` — direct `File::Exists`. Per `Fs.md`'s Error Handling
  Policy (unconditional: every `Result<T>`/`Result<void>` failure
  throws), a genuine `File::Exists` failure still throws here — a
  deliberate divergence from Node's own `fs.existsSync`, which swallows
  every error and returns `false`. `Fs.md`'s Design Goals explicitly
  permit divergence from Node's exact behavior wherever `forge-core`'s
  semantics differ, and they do here: `forge-core` distinguishes
  "doesn't exist" (`false`, not an error) from "couldn't tell" (an
  `Error`), and this binding preserves that distinction instead of
  collapsing it the way Node does.
- `FsMkdirSync` — `options.recursive` selects `File::CreateDirectories`
  ("mkdir -p", tolerates an already-existing ancestor including the
  target itself, per its own internal `AlreadyExists`-tolerant loop) vs.
  the non-recursive `File::MakeDirectory` (fails with `AlreadyExists` if
  the target itself already exists, `NotFound` if any ancestor is
  missing — confirmed via `File.cpp`'s real `TranslateWin32Error` calls,
  not guessed).
- `FsRmSync` — direct `File::Remove` (Win32 `DeleteFileW`), file-removal
  only per `Fs.md`'s Non-Goals.
- `FsStatSync` — opens the file (`FileMode::Read`, so a missing path
  throws `NotFound` the same way `readFileSync` does), reads
  `SizeInBytes()`, closes it, and returns `{ size }` via a fresh
  `JS_NewPlainObject` + `JS_DefineProperty` — the entire shape `Fs.md`
  specifies (no `isFile`/`isDirectory`/timestamps, since `File.h` has no
  API to back them).
- `DefineFsNamespace` — creates `globalThis.fs` (`JS_NewPlainObject` +
  `JS_DefineFunction` per method + `JS_DefineProperty` on the global),
  called once from `main()` right after the existing flat-global
  `JS_DefineFunction` calls, per `JsBindings.md`'s namespaced-global
  naming convention.

Two new includes were needed and are not covered by Phase 7.2's own
verification: `js/PropertyDescriptor.h` (for `JSPROP_ENUMERATE`,
`statSync`'s `{size}` property) and `jsapi.h` (for `JS_NewPlainObject`,
which — confirmed by reading `js/public/Object.h` directly — is not
exposed under `js/public` at all; it lives in the classic top-level
`js/src/jsapi.h` embedding header instead, confirmed by reading that
file directly too, not guessed).

**Smoke tests.** A 17-case addition to `forge --self-test`
(`RunFsSmokeTests`, run immediately after `RunMarshallingSmokeTests`),
covering every method with at least one success path and one failure
path, per `Fs.md`'s own Acceptance Criteria: `writeFileSync`(string)+
`readFileSync`(bytes) round trip, `readFileSync`(`"utf8"`),
`writeFileSync`(`Uint8Array` data), `appendFileSync` concatenation,
`existsSync` true/false, `mkdirSync` non-recursive and recursive (nested
ancestors), `rmSync` removing a file, `statSync` reporting size, and
seven failure cases (`readFileSync`/`rmSync`/`statSync`/`mkdirSync`
against a missing path or missing parent → `NotFound`;
`writeFileSync`(a number) → `InvalidArgument`; `appendFileSync` against
a missing parent → `NotFound`; `existsSync`(a `Symbol`) →
`InvalidArgument`, exercising the path-conversion failure rather than
`File::Exists` itself, since forcing a genuine `File::Exists`-level
failure isn't portably reproducible from a self-test — documented as
such in the test's own comment rather than presented as something it
isn't). Fixtures are deterministic, fixed-name paths under a
`forge-selftest-fs-scratch/` directory (no `Date.now()`/`Math.random()`)
so repeated runs don't accumulate garbage; the one non-idempotent case
(`mkdirSync` without `recursive` on an already-existing directory throws
`AlreadyExists` the second time a run happens against the same on-disk
state) is handled by the test itself tolerating that one specific,
documented outcome via a `try`/`catch` checking `e.code`, rather than
being treated as a failure.

**A real fix made in the same pass, not flagged by the user:**
`RunMarshallingSmokeTests` used to print its own
`[self-test] ALL PASSED`/`FAILURES ABOVE` summary line internally, at
the end of its own loop. Now that `--self-test` runs both suites in
sequence, that would have printed a possibly-false "ALL PASSED" before
`RunFsSmokeTests` (which runs afterward) had any chance to fail — caught
during this same implementation pass and fixed by moving the combined
summary print to `main()`, after both suites have run and their results
are ANDed together.

**Verification before delivery.** Since this sandbox cannot run real
`JS::Evaluate` against multi-statement JS or real property-based
`fs.xxx()` dispatch (the fake JSAPI shim used for Phase 7.2 only
pattern-matches simple literal-value expressions, not a snippet that
calls into a bound method), verification here took two forms rather
than one:

1. **Compile check** of the entire new section (all seven binding
   functions, `ResolveWriteData`, `WriteAllBytes`, `DefineFsNamespace`,
   all 17 self-test cases, `RunFsSmokeTests`) against the real
   `forge-core` headers and an extended fake JSAPI shim (adding
   `JS_NewPlainObject`, the `JS::Handle<JSObject*>`/`double`-value
   `JS_DefineProperty` overloads, `JSPROP_ENUMERATE`, `JS::ToBoolean`,
   and a real-`JS::CallArgsFromVp`-compatible `JS::CallArgs` mirroring
   the actual `vp[-2]`/`vp[-1]` slot-sharing layout, not just the public
   method surface) under g++ and clang++
   (`-std=c++17 -Wall -Wextra -Wpedantic -Werror`) — 0 warnings, 0
   errors. This caught one real bug before it reached the user: an
   unused `JSContext* cx` parameter on the fixture-setup helper
   `SelfTest_FsEnsureScratchDir` (it doesn't touch the context at all)
   that would have failed the real build under `-Werror` — fixed by
   dropping the parameter and updating its one call site.
2. **Direct-`CallArgs` logic verification.** A separate sandbox-only
   harness (never delivered) invokes all seven `Fs*Sync` functions
   directly — constructing a `JS::Value* vp` array matching the real
   engine's calling convention, the same way a real embedder's own C++
   code would invoke a `JSNative` without going through script
   evaluation at all — against a POSIX (not Win32) re-implementation of
   `File`'s backend written for this harness only, giving the binding
   logic real file I/O to run against on Linux. 20/20 checks pass:
   every method's success and failure path, plus `DefineFsNamespace`
   itself. The full Phase 7.2 suite was re-run in the same binary as a
   regression check: still 12/12. All of this also passed clean under
   ASan+UBSan (0 errors with `detect_leaks=0`; with leak detection on,
   every leak traces to the fake JSAPI shim's own intentional non-
   freeing of simulated engine objects — the same finding Phase 7.2's
   own verification already made, not a new issue) and valgrind
   memcheck (`ERROR SUMMARY: 0 errors from 0 contexts`).

What neither of the above covers, and cannot, in this sandbox: the real
Win32 `File` backend (`File.cpp` itself is already only "verified by
careful manual review only" pending a real build — an existing,
accepted limitation from Phase 3, not new here) and real
`JS::Evaluate`/property-based `fs.xxx()` dispatch through an actual
SpiderMonkey engine. `RunFsSmokeTests` itself was still run in this
sandbox (for structural/control-flow coverage — the scratch-directory
setup, the loop over all 17 cases), but every one of its cases reports
`FAIL` there, which is an expected, documented limitation of the fake
JS::Evaluate, not a sign of a real bug — the direct-`CallArgs` harness
above is what actually verifies this phase's logic in the sandbox.

No design deviation from the frozen `Fs.md`/`JsBindings.md` specs was
needed.

**Status (as of this entry): Phase 7.2 (all of it) is now fully
real-build-confirmed. Phase 7.3 is implemented and sandbox-verified only
so far — one more real `python mach build`, followed by
`forge --self-test` printing `[self-test] ALL PASSED` (29 cases total:
the original 12 plus these 17) and exiting 0, is what closes this out
and clears the way for Phase 7.4 (benchmarks) or a future phase.** See
the following entry for that confirmation.

## Phase 7.3 fully real-build-confirmed (2026-07-30)

The user ran `python mach build` a third time, against this exact
delivered `forge.cpp` (unchanged since the previous entry — no new
edits were made between that sandbox-verified delivery and this build),
and it succeeded. `forge --self-test` printed `[self-test] ALL PASSED`
across all 29 cases, exit code 0: the original 12 Phase 7.2 cases plus
all 17 new `fs.*Sync` cases (`RunFsSmokeTests`), covering at least one
success and one failure path for each of `readFileSync`, `writeFileSync`,
`appendFileSync`, `existsSync`, `mkdirSync`, `rmSync`, and `statSync`,
per `Fs.md`'s own Acceptance Criteria.

Phase 7.3 is therefore now fully real-build-confirmed: every method in
`Fs.md`'s Public API is wired up via `DefineFsNamespace` and exercised by
a passing self-test case on the real Gecko/SpiderMonkey build, not just
the sandbox harness described in the previous entry. That sandbox work
remains valuable and is kept in the record above (it caught a real
`-Werror` bug pre-build — the unused `SelfTest_FsEnsureScratchDir`
parameter — and is the only place this logic has been exercised under
ASan/UBSan/valgrind), but it is no longer the basis for Phase 7.3's
completion status; the real build is.

No design deviation from the frozen `Fs.md`/`JsBindings.md` specs was
needed, and nothing about this build required touching `forge.cpp`
again.

**Status: Phase 7.1, 7.2, and 7.3 are all now fully real-build-confirmed.
Phase 7.4 (benchmarks, starting with `bench/fs-bench.js` run through the
existing `run-benchmarks.ps1` harness) is next.**

## Phase 4 (Threading) resumed: moz.build fix re-confirmed, two latent smoke-test bugs caught (2026-07-30)

With Phase 7 fully done, the user chose Phase 4 (real-build-confirming
`Mutex`/`ConditionVariable`/`Thread`/`ThreadPool`) as the next milestone
over Phase 5 (Networking) — Phase 4 is earlier in `ROADMAP.md`'s own
ordering, is pure verification debt (no new code needed), and unblocks
both Phase 7.5 (async `fs`, gated on `ThreadPool`) and any future
worker-thread JS bindings, more than Phase 5 would unblock on its own.

Before asking for another real `mach build`, re-checked what was
actually already true on disk rather than trusting the existing
`ROADMAP.md` prose at face value:

- Re-read the live `moz.build` directly off the device and confirmed
  programmatically (`sorted(sources, key=str.lower)` in a throwaway
  script, not by eye) that its current `SOURCES` list already matches
  case-insensitive sorted order — the fix recorded in the "`moz.build`
  SOURCES ordering bug" entry above is genuinely in place, not merely
  described. This is a different situation from the
  `Forge_Benchmark_Report.md` discrepancy found in Phase 7.4 — here, the
  doc's claim checked out.
- Listed `forge-core/` recursively off the device and confirmed
  `ThreadingSmokeTest.cpp`, `Mutex.cpp`/`.h`, `ConditionVariable.cpp`/
  `.h`, `Thread.cpp`/`.h`/`.inl`, `ThreadPool.cpp`/`.h`/`.inl`, and
  `platform/Win32Error.cpp`/`.h` all exist as described.

While reading `ThreadingSmokeTest.cpp` and (for comparison)
`FileSmokeTest.cpp` to confirm the exact build command to hand back to
the user, found a real latent bug in both files' own header-comment `cl`
build commands, never previously exercised (neither smoke test has
actually been built on the real machine yet): each command mixes
unprefixed paths (e.g. `Mutex.cpp`, `File.cpp`, `platform\Win32Error.cpp`
— correct only if invoked from within `forge-core\`, where both smoke
test files themselves live) with a redundant `forge-core\`-prefixed path
for `memory\DefaultAllocator.cpp`/`memory\detail\AllocationBackend.cpp`
in that same command line. Run verbatim from `forge-core\` — the natural
reading, since every other path in the same line has no directory
prefix — the two `DefaultAllocator`/`AllocationBackend` paths would
have resolved to `forge-core\forge-core\memory\...`, a file-not-found
error. Fixed both comments to drop the redundant prefix, matching every
other path in the same command. `SocketSmokeTest.cpp`'s build comment
has the identical bug; left untouched since Phase 5 isn't in scope this
round, but noted in `ROADMAP.md` so it isn't a surprise whenever that
phase is picked up.

No production code changed in this entry — `Mutex.cpp`/
`ConditionVariable.cpp`/`Thread.cpp`/`ThreadPool.cpp`/`Win32Error.cpp`/
`moz.build` are exactly as they were when Phase 4 was last touched. Only
`ThreadingSmokeTest.cpp`'s and `FileSmokeTest.cpp`'s header comments
changed.

**Status: nothing here is real-build-confirmed yet — this entry is
re-verification-of-claims plus a doc-comment fix, not new evidence about
`Mutex`/`ConditionVariable`/`Thread`/`ThreadPool` themselves.** Two real
steps remain, both on the user's real machine: (1) `python mach build`,
the first real attempt against the now-believed-correct `moz.build`
order; (2) build and run `ThreadingSmokeTest.cpp` per its corrected
comment, from within `forge-core\`. See `ROADMAP.md`'s Phase 4 entry for
the exact commands and expected output. Phase 4 stays open until both
are reported back.

## Phase 4: real MSVC compile of production sources succeeds; a real MSVC-specific bug in ThreadingSmokeTest.cpp caught and fixed (2026-07-30)

The user ran the corrected `cl` command from the previous entry, from
`forge-core\` in a Visual Studio Developer PowerShell. All seven
production sources it references — `Mutex.cpp`, `ConditionVariable.cpp`,
`Thread.cpp`, `ThreadPool.cpp`, `platform\Win32Error.cpp`,
`memory\DefaultAllocator.cpp`, `memory\detail\AllocationBackend.cpp` —
compiled clean under `/std:c++17 /EHs- /W4` against the real
`<windows.h>`. This is genuinely new evidence, distinct from anything
recorded earlier in this file: every prior verification pass for these
five Win32-backed files (`Mutex`/`ConditionVariable`/`Thread`/
`ThreadPool`/`Win32Error`) used a hand-written mock `<windows.h>`,
which can catch typos and signature mismatches but proves nothing about
whether the code is actually correct against the real SDK. This compile
is the first time that's been checked for real.

Only `ThreadingSmokeTest.cpp` itself failed:

```
ThreadingSmokeTest.cpp(67): error C3493:
'kIterations' cannot be implicitly captured because no default capture mode has been specified
```

Root cause: `RunMutexStressTest`'s worker lambda —
`[&mutex, &counter]() { for (int j = 0; j < kIterations; ++j) ... }` —
reads the enclosing `constexpr int kIterations` without capturing it.
Per the C++17 standard, a `constexpr` local that's only read (never
odr-used — no reference or address taken) doesn't need to appear in a
lambda's capture list, and that's exactly why this compiled clean under
g++ and clang++ in every sandbox pass this session. MSVC's frontend
doesn't extend that exemption the same way and requires the capture
regardless — a real, compiler-specific portability gap, not a logic bug
in the test itself. Fixed by adding `kIterations` to the capture list:
`[&mutex, &counter, kIterations]`. Reviewed every other lambda in the
file for the same pattern (`RunConditionVariableProducerConsumerTest`'s
`[&]()`, `RunThreadPoolTest`'s `[&mutex, &completed]()` and `[]() {}`) —
none of the others reference an uncaptured local, so this was the only
instance.

Since the smoke test failed to compile, linking never completed and
`ThreadingSmokeTest.exe` was never produced — none of the real-thread
behavior (`Mutex` mutual exclusion, `ConditionVariable` handoff/timeout,
`ThreadPool` task racing) has been exercised yet. That's still the
single biggest remaining unknown for this phase.

**Status:** production Win32 threading code compiling clean against the
real `<windows.h>` under a real compiler is real progress, but it is not
the same claim as "`python mach build` succeeded" — the manual `cl`
invocation bypasses `moz.build` entirely, so it says nothing about
whether the case-insensitive `SOURCES` sort fix actually lets `mach
build`'s own frontend parse the file and proceed to compilation, which
is what this project has consistently meant by "real-build-confirmed"
for every other phase. The user's message describing this round didn't
mention running `python mach build` itself, so that is being treated as
still open, not assumed — asked directly rather than guessed at. Two
things remain: rebuild and run `ThreadingSmokeTest.cpp` with the capture
fix (same command as before), and run `python mach build`. Phase 4 stays
open until both are reported.

## `ThreadingSmokeTest.cpp` passes against real OS threads (2026-07-30)

The user rebuilt `ThreadingSmokeTest.cpp` with the `kIterations`-capture
fix from the previous entry — clean compile, no errors this time — and
ran the resulting `.exe`. Output:

```
RunMutexStressTest...
RunConditionVariableProducerConsumerTest...
RunConditionVariableTimeoutTest...
RunThreadPoolTest...
ThreadingSmokeTest: all checks passed
```

All four scenarios passed against real OS threads:

- `RunMutexStressTest`: 8 threads × 20,000 increments each under a
  single `Mutex`, final count came out exactly `160000` — real mutual
  exclusion under actual OS scheduling, not a lucky race (this test is
  specifically designed to reliably fail without correct locking).
- `RunConditionVariableProducerConsumerTest`: the producer/consumer
  handoff completed correctly (`payload == 42`, `ready == true` after
  the consumer thread woke from `Wait()`).
- `RunConditionVariableTimeoutTest`: `WaitFor` correctly returned
  `false` after actually timing out with nobody notifying.
- `RunThreadPoolTest`: all 500 submitted tasks across 4 real worker
  threads completed exactly once each, and `Submit()` after `Shutdown()`
  correctly failed rather than crashing or silently succeeding.

This is the real-thread evidence that no sandbox verification could ever
provide (the mock `CreateThread` used throughout this project's earlier
sandbox passes is a dumb stub that can't exercise real scheduling,
blocking, or waking). Combined with the previous entry's real MSVC
compile of the production sources against the real `<windows.h>`, every
piece of Phase 4's Win32-backed code now has real evidence behind it
except one: `python mach build` itself, through Gecko's own build
integration, still hasn't been run. The user has said they'll run it
next.

**Status: not yet marking Phase 4 real-build-confirmed.** Per this
project's own consistent standard (every other phase's
"real-build-confirmed" label has specifically meant `mach build`
succeeded, not a standalone `cl` compile), this label is being held
until that result is reported — even though the smoke test result above
is itself very strong evidence the underlying implementation is
correct. Will update `ROADMAP.md`/`HISTORY.md`/`PROJECT_CONTEXT.md` to
close this out the moment `mach build`'s result comes back.

## Phase 4 fully real-build-confirmed: `python mach build` succeeds (2026-07-30)

The user ran `python mach build` after the `ThreadingSmokeTest.cpp`
capture fix, and it completed successfully. This is the piece that had
been deliberately held open across the last three entries: the manual
`cl` compile and the smoke test run both bypass `moz.build`, so neither
one could confirm the case-insensitive `SOURCES` sort fix (from the
"`moz.build` SOURCES ordering bug" entry) actually resolves the parse
failure and lets `mach build`'s own frontend proceed to compiling
`Mutex.cpp`/`ConditionVariable.cpp`/`Thread.cpp`/`ThreadPool.cpp`/
`platform/Win32Error.cpp` for real. It now has.

Phase 4 is complete across all three legs of verification, together:

1. `python mach build` — real Gecko/`moz.build` toolchain integration,
   now confirmed.
2. A standalone MSVC `cl` compile of the same five files against the
   real `<windows.h>` (not a mock) — confirms the Win32 API usage
   itself.
3. `ThreadingSmokeTest.cpp` (rebuilt, run) — confirms real-thread
   behavior: mutual exclusion under real contention, condition-variable
   wait/notify/timeout, and a thread pool running every task exactly
   once and correctly rejecting work post-shutdown.

No design deviation was needed anywhere in this phase. The two real bugs
this phase's rewind uncovered — the friend-access mistake in an early
`ConditionVariable.cpp` draft, and the rollback-path deadlock risk in
`ThreadPool::Initialize` (both from the original Phase 4 implementation
entry) — are both confirmed fixed by the smoke test passing cleanly
(the mutex/condition-variable/thread-pool logic those fixes touch is
exactly what `ThreadingSmokeTest.cpp` exercises). The `moz.build`
sort-order bug and the `ThreadingSmokeTest.cpp` MSVC capture bug are
also both confirmed fixed, now that the commands that originally failed
on each have both succeeded.

**Status: Phase 4 — `Mutex`, `ConditionVariable`, `Thread`, `ThreadPool`,
`ErasedCallable`, `LockGuard<T>` — is fully real-build-confirmed. No
sandbox-only claim remains anywhere in this phase.** Per the user's own
stated priority this round (real-build debt before new feature work),
the two oldest pieces of unresolved verification debt in the project are
now both addressed: Phase 3 (`File`) and Phase 4 (`Threading`) are both
fully real-build-confirmed, alongside Phase 6 and Phase 7 from earlier
this session. Only Phase 5 (Networking — `Socket.cpp` still not even in
`moz.build`) remains as real-build debt from before Phase 6.

## Phase 5 (Networking) resumed: `moz.build` wiring completed, two more real bugs caught in `SocketSmokeTest.cpp` (2026-07-30)

With Phase 4 fully closed, the user asked to continue finishing real-
build verification debt (over starting new feature work) — Phase 5 is
the only phase left with any. Before touching anything, re-confirmed
what was actually true on disk rather than trusting `ROADMAP.md`'s prior
"not started" note at face value: `Socket.cpp`/`Socket.h`,
`IpAddress.h`/`.inl`, `Socket.md`/`IpAddress.md`, and `SocketSmokeTest.cpp`
all exist and were read in full. The implementation itself held up under
review — `Socket.cpp`'s Winsock usage (`WSAStartup`/`socket`/`connect`/
`bind`/`listen`/`accept`/`send`/`recv`/`closesocket`, all through
`TranslateWinsockError`) matches `Socket.md`'s frozen spec exactly, and
`platform/Win32Error.h`/`.cpp` already had `TranslateWinsockError`
implemented (added alongside `Socket.cpp` originally, per its own doc
comment), so no dependency was missing there.

Two concrete gaps found and fixed:

1. **`Socket.cpp` was never in `moz.build`'s `SOURCES`.** Added it, in
   the correct case-insensitive sorted position (verified
   programmatically, same as the Phase 4 sort fix): between
   `platform/Win32Error.cpp` and `Thread.cpp`.
2. **No `OS_LIBS` entry for `ws2_32` existed either**, even though
   `Socket.cpp`'s own comment already assumed one would be added
   "alongside this file" — it never was. Without it, the real `mach
   build`'s link step could plausibly fail with unresolved externals for
   `WSAStartup`/`socket`/etc., a different and later failure mode than
   the missing-`SOURCES`-entry problem, and not something the file's own
   `#pragma comment(lib, "ws2_32.lib")` could be relied on alone to fix
   inside Gecko's own build wrapper. Added `OS_LIBS += ["ws2_32"]`.

While preparing `SocketSmokeTest.cpp`'s build instructions (it has never
been run, same situation `ThreadingSmokeTest.cpp` was in before this
session), found two more real, previously-uncaught bugs:

- **The identical MSVC lambda-capture bug just fixed in
  `ThreadingSmokeTest.cpp`.** `RunSocketEchoTest`'s server-thread lambda
  reads `constexpr Size kPayloadSize` (an array bound, and a `Span`
  argument passed by value — confirmed by reading `Span`'s constructor
  signature, which takes `SizeType` by value, so this is a pure read, not
  an odr-use) without capturing it. Same GCC/Clang-exempts-it,
  MSVC-doesn't situation as before. Fixed by adding `kPayloadSize` to the
  capture list.
- **A real bug in the file's own example build command**: it listed
  `IpAddress.inl` as a source file to compile directly, alongside
  `Socket.cpp`. Every other `.inl` in this codebase (`Path.inl`,
  `Queue.inl`, etc.) is `#include`d by its own header — `IpAddress.h`
  does this at its own end — not compiled as an independent translation
  unit; `IpAddress.inl`'s member-function definitions reference
  `IpAddress`/`Endpoint`, which don't exist yet at that point unless
  `IpAddress.h` has already been processed. Compiling it standalone would
  have failed with undeclared-identifier errors, wasting the user's time
  on a spurious compile failure unrelated to `Socket`/`Mutex`/`Thread`
  themselves. Removed it from the command — `Socket.cpp`'s own
  `#include "IpAddress.h"` already provides everything needed. Also fixed
  the same redundant `forge-core\`-prefix path bug found in
  `ThreadingSmokeTest.cpp`/`FileSmokeTest.cpp` in the same pass.

No production `Socket.cpp`/`Socket.h`/`IpAddress.h`/`IpAddress.inl` code
changed in this entry — only `moz.build` and `SocketSmokeTest.cpp`'s
header comment and one lambda's capture list.

**Status: Phase 5 is not yet real-build-confirmed — this entry is
wiring plus bug-fixing, not new evidence that `Socket` itself works
against a real TCP stack.** Two real steps remain, both on the user's
machine: (1) `python mach build`, the first real attempt at compiling
`Socket.cpp` through the now-corrected `moz.build`; (2) build and run
`SocketSmokeTest.cpp` per its corrected comment, from `forge-core\`. See
`ROADMAP.md`'s Phase 5 entry for exact commands and expected output.

## Real MSVC compile of Socket.cpp succeeds; the `kPayloadSize` fix from the previous entry was itself wrong, now corrected (2026-07-30)

The user ran the corrected `cl` command. All seven production sources —
`Socket.cpp`, `Mutex.cpp`, `ConditionVariable.cpp`, `Thread.cpp`,
`platform/Win32Error.cpp`, `memory/DefaultAllocator.cpp`,
`memory/detail/AllocationBackend.cpp` — compiled clean, including
`Socket.cpp` against the real `<winsock2.h>`/`<ws2tcpip.h>` for the first
time ever (every prior pass used a hand-written mock). Real, new
evidence, same category as the equivalent Phase 4 milestone.

`SocketSmokeTest.cpp` itself failed:

```
SocketSmokeTest.cpp(142): error C2131: expression did not evaluate to a constant
note: failure was caused by reading a value that is unknown during constant evaluation
```

This is a direct consequence of the previous entry's own fix being
incomplete — actually wrong in a subtler way. Adding `kPayloadSize` to
the lambda's capture list *did* satisfy MSVC's stricter odr-use rule
(matching `ThreadingSmokeTest.cpp`'s `kIterations` fix), but it also
changed what the name `kPayloadSize` refers to *inside* the lambda body:
once a variable is captured, every use of its name inside the lambda
denotes the closure type's own copied data member, not the original
enclosing `constexpr`. That member is an ordinary runtime value, not a
compile-time constant — so `u8 received[kPayloadSize]{}` (line 142, an
array bound, which must be a real constant expression) stopped
compiling, even though the exact same name compiled fine as a loop bound
in `ThreadingSmokeTest.cpp` (a for-loop condition doesn't require a
constant expression at all, only a value). Reading a value copied from a
constexpr is not the same as reading the constexpr itself once a
capture is involved — a real, subtle distinction neither GCC nor Clang's
sandbox pass could have surfaced (they don't even require capturing a
read-only `constexpr` in the first place, so the question of what its
name resolves to *after* capturing never came up there).

**Fix:** stopped treating this as a capture problem at all. Moved
`kPayloadSize` out of `RunSocketEchoTest` (a local) to file scope inside
the anonymous namespace (a `constexpr Size kPayloadSize = 5;` alongside
`g_failures`). A file-scope constant is never a "local variable of an
enclosing function" in the first place, so no lambda anywhere in the
file needs to capture it, and every use — array bound or plain value —
refers to the same genuine constant expression, with no capture-vs-
original-entity ambiguity possible. Removed `kPayloadSize` from the
lambda's capture list (now just `[listenerSocket = ..., &serverOk]`) and
removed the now-redundant local declaration.

No other lambda or lambda-adjacent constant use in this file follows the
same pattern (verified by re-reading the whole file end to end again).

**Status: still not real-build-confirmed.** Two real steps remain,
unchanged from the previous entry: rebuild `SocketSmokeTest.cpp` with
this fix and run the resulting `.exe`, and run `python mach build`. The
user has not yet reported either succeeding with this fix in place.

## Phase 5 fully real-build-confirmed: `SocketSmokeTest.cpp` passes, `python mach build` succeeds (2026-07-30)

The user rebuilt `SocketSmokeTest.cpp` with the file-scope
`kPayloadSize` fix — clean compile, no errors — and ran the resulting
`.exe`:

```
RunSocketEchoTest...
RunSocketConnectRefusedTest...
SocketSmokeTest: all checks passed
```

`RunSocketEchoTest` exercised a real loopback TCP connection end to end:
`Socket::Listen` on `127.0.0.1:53421`, a background `Thread` calling
`Accept()`, the main thread as client calling `Connect()`, a 5-byte
payload sent and echoed back, and the echoed bytes verified byte-for-
byte against the original — real `Connect`/`Listen`/`Accept`/`Send`/
`Receive`/`Close` behavior against an actual OS TCP stack, not a mock.
`RunSocketConnectRefusedTest` confirmed `Connect()` to a port nothing is
listening on fails cleanly as a `Result` error rather than crashing or
hanging.

Separately, the user ran `python mach build` and it completed
successfully — the first time `Socket.cpp` has been compiled and linked
through Gecko's own toolchain integration, confirming both the
`SOURCES` addition and the new `ws2_32` `OS_LIBS` entry from earlier in
this phase actually resolve correctly at link time (the concern that
entry was specifically added to address).

Phase 5 is complete across all three legs of verification, together:

1. `python mach build` — real Gecko/`moz.build` toolchain integration,
   including linking against `ws2_32`, now confirmed.
2. A standalone MSVC `cl` compile of the same production sources against
   the real `<winsock2.h>`/`<ws2tcpip.h>` (not a mock).
3. `SocketSmokeTest.cpp` (rebuilt, run) — confirms real network
   behavior: an actual loopback TCP connection, byte-accurate echo, and
   a real connection-refused failure path.

No design deviation was needed anywhere in this phase — `Socket.cpp`'s
implementation matched `Socket.md`'s frozen spec exactly on first real
compile. The three real issues this phase's resumption surfaced (the
missing `SOURCES` entry, the missing `OS_LIBS` entry, and
`SocketSmokeTest.cpp`'s two bugs — the `IpAddress.inl` build-command
error and the `kPayloadSize` capture/file-scope issue) are all confirmed
fixed by this pass succeeding cleanly.

**Status: Phase 5 — `Socket`, `IpAddress` — is fully real-build-
confirmed. No sandbox-only claim remains anywhere in this phase.** With
Phase 3, 4, and 5 now all fully real-build-confirmed alongside Phase 6
and 7 from earlier this session, every phase in this project's history
has real-build evidence behind it — no outstanding real-build debt
remains anywhere in `ROADMAP.md` as of this entry.

## Phase 7.5 (Async fs) chosen as next milestone; design frozen (2026-08-02)

With Phases 1-7 all fully real-build-confirmed, the user asked to move
to new functionality and requested a review of roadmap options before
committing to one, explicitly deferring the `promise-chain-bench.js`
performance gap as a separate profiling task for later (real profiling
data, not guessing). Four candidates were presented: async `fs` (Phase
7.5), `net.*` JS bindings, worker-thread bindings, and a minimal module
system. The user chose async `fs`, then explicitly reiterated the
project's established per-phase workflow for it: freeze the design,
implement incrementally, verify each milestone with `python mach build`,
add self-tests, benchmark and update docs — the same shape Phase 7 itself
followed (7.1 design freeze, 7.2/7.3 implementation, 7.4 benchmarks).

`FsAsync.md` was written as the frozen-candidate design spec, following
`JsBindings.md`/`Fs.md`'s established section shape, grounded in direct
reads of `IocpLoop.h`, `ThreadPool.h`, `forge.cpp`'s `Runtime` class, and
`IocpSmokeTest.cpp` rather than assumption. Three previously-unflagged
risks came directly out of that grounding, not out of the abstract spec
shape:

1. **`Runtime::run()`'s event-loop exit condition
   (`jobQueue_.empty() && loop_.Empty()`) has a real, pre-existing gap.**
   A script whose only pending work is an in-flight
   `fs.readFile(...).then(...)`, with no timer also pending, would see the
   process exit before the read ever completes — `IocpLoop::Empty()`'s
   own doc comment had already predicted exactly this ("a future async
   ... variant... will need to add an active-operation count"). The fix
   is scoped to `Runtime` itself (a pending-operation counter,
   `forge.cpp`), not `IocpLoop` (frozen, real-build-confirmed).
2. **Cross-thread `PostCompletion` has never actually been tested.**
   `IocpSmokeTest.cpp`'s only real-machine evidence for `PostCompletion`
   is a same-thread post-then-immediately-`RunOnce()` pattern. Nothing has
   ever exercised the genuine cross-thread case (one thread blocked inside
   `RunOnce()`'s wait while a different thread posts) that this phase
   fundamentally depends on. Flagged as the single biggest open technical
   risk, with a dedicated real-machine smoke test required before
   anything else in this phase is built on top of the assumption that it
   works — this project's established bar (`File`, `Mutex`/
   `ConditionVariable`, `Socket` all needed their own real-machine smoke
   test despite being equally well-documented Win32 APIs) doesn't get
   quietly lowered here just because Microsoft's own docs say it should
   work.
3. **Native `JS::Promise` construction/resolution is new binding
   surface.** Confirmed via grep that no `JS::NewPromiseObject`/
   `ResolvePromise`/`RejectPromise` call exists anywhere in this codebase
   yet — every existing use of `Promise` (e.g. `promise-chain-bench.js`)
   is pure script calling the engine's own built-in `Promise`.

Also settled: write/append data must be **copied** into an owned buffer
before dispatch, never aliased across the thread boundary (the existing
sync-path `AsByteSpan`/`ResolveWriteData` contract is only valid "for the
duration of the call"); the returned `Promise` must be GC-rooted
independent of whether script retains a reference to it; and the existing
self-test harness (Phase 7.2/7.3's `RunFsSmokeTests`, which calls binding
functions directly and inspects a `Result<T>` synchronously) cannot cover
this phase as-is — Promise-based self-tests need `Runtime::run()` to
actually drive the event loop to completion, flagged as an
implementation-stage question rather than assumed solvable by reusing the
existing harness unchanged.

The user reviewed `FsAsync.md` and explicitly confirmed all of the above
as the phase's committed design, with no remaining unresolved questions.
**Design frozen 2026-08-02.** Nothing was implemented before the freeze.

## Phase 7.5.1: cross-thread `PostCompletion` smoke test written (2026-08-02)

Per the frozen design's own call-out that the cross-thread `PostCompletion`
pattern is this phase's single biggest open risk, that risk is addressed
first, before any of the actual worker-dispatch/`Promise`-settlement
machinery is built on top of the assumption that it works.
`IocpCrossThreadSmokeTest.cpp` (new file, same "separate console-app
project, not part of `moz.build`" pattern as every other `*SmokeTest.cpp`
in this codebase): initializes a real `IocpLoop`, schedules a 5-second
non-repeating safety-net timer (so a genuine failure of the cross-thread
pattern produces a bounded-time, clearly-reported FAILURE rather than an
indefinite hang if it doesn't work), spawns a real `Thread` (Phase 4) that
sleeps 200ms and then calls `PostCompletion` from that thread, while the
main thread calls `RunOnce()` with nothing already posted — so it
genuinely blocks inside `GetQueuedCompletionStatus`'s wait, the exact
cross-thread pattern `IocpSmokeTest.cpp` never covered.

One subtlety worth recording: the worker thread's `PostCompletion` call is
followed, in the lambda, by a check of its own result — but that check
must not touch the shared, non-atomic `g_failures` the `CHECK` macro
mutates, since nothing synchronizes "worker calls `PostCompletion`" with
"worker's very next statement" the way IOCP's own completion handoff
synchronizes the `PostCompletion` call itself with `RunOnce()` waking up
on the main thread — only `Thread::Join()` afterward provides that
synchronization. The test instead records the outcome in a plain
`workerPostFailed` bool, read on the main thread only after `Join()`.

**Not yet real-machine-confirmed.** This needs to be built and run on the
real machine, same as every other smoke test in this codebase, before
7.5.2 (the `Runtime` pending-operation counter) or 7.5.3 (the actual
per-operation dispatch/`Promise` machinery) can start.

## Phase 7.5.1 real-machine-confirmed: cross-thread `PostCompletion` works (2026-08-02)

The user built `IocpCrossThreadSmokeTest.cpp` on the real machine. The
first attempt, using this file's own documented `cl` command verbatim,
failed at link time with unresolved `IocpLoop::*`/`TimerScheduler::*`
symbols — a real gap in the build comment, not a code bug: `IoLoop.h`
only pulls in `IocpLoop`'s (and transitively `TimerScheduler`'s)
declarations, the same way every other header in this codebase separates
declaration from definition, so the two `.cpp` files providing those
definitions (`forge-core/platform/IocpLoop.cpp` and
`forge-core/platform/TimerScheduler.cpp`) needed to be listed alongside
`Thread.cpp` and the rest — they weren't. Fixed by adding both to the
documented build command.

With that fix, the build succeeded and the test passed:

```
IocpCrossThreadSmokeTest: loop initialized
IocpCrossThreadSmokeTest: worker thread posting completion after delay...
IocpCrossThreadSmokeTest: RunOnce() woke up (bytes=99, key=7, ok=1)
IocpCrossThreadSmokeTest: all checks passed
```

The 5-second safety-net timer did not fire — `RunOnce()` was woken
directly by the worker thread's cross-thread `PostCompletion` call, the
exact pattern under test. This is real, on-the-metal confirmation that
IOCP's documented cross-thread wakeup behavior holds on this machine, not
just Microsoft's own documentation being taken on faith — matching the
bar this project has held every other well-documented Win32 API to
(`File`, `Mutex`/`ConditionVariable`, `Socket`).

**Status: Phase 7.5's single biggest open technical risk, as identified
in `FsAsync.md`'s Design Goals, is now closed.** 7.5.2 (the `Runtime`
pending-operation counter and event-loop lifetime fix) can begin.

## Phase 7.5.2: `Runtime` pending-operation counter + event-loop lifetime fix implemented (2026-08-02)

Adds the `pendingAsyncOps_` counter and `OnAsyncOperationStarted()`/
`OnAsyncOperationFinished()` methods to `Runtime` (`forge.cpp`), and
extends `run()`'s exit condition from `jobQueue_.empty() &&
loop_.Empty()` to `jobQueue_.empty() && loop_.Empty() &&
pendingAsyncOps_ == 0` — the fix `FsAsync.md`'s Design Goals called for,
closing the gap `IocpLoop::Empty()`'s own doc comment had already
predicted ("a future async... variant... will need to add an
active-operation count"). `IocpLoop.h`/`.cpp` are untouched, matching the
design's Dependencies section (`IocpLoop`'s public API stays frozen; the
bookkeeping lives on `Runtime` instead, which `Empty()`'s own comment
already deferred to the caller).

`OnAsyncOperationFinished()` asserts (via `FORGE_ASSERT`, newly included
from `forge-core/Assert.h`) that the counter is nonzero before
decrementing, rather than silently wrapping/clamping on a mismatched
Started()/Finished() pair — matching the same "loud failure over silent
misbehavior" convention `Thread.h`'s `Joinable()`/destructor contract
already uses elsewhere in this codebase.

Worth being explicit about: **this change cannot alter any existing
behavior yet, by construction.** Nothing in the codebase calls
`OnAsyncOperationStarted()` anywhere until 7.5.3 gives it a real caller,
so `pendingAsyncOps_` is provably always `0` today, and the new
`&& pendingAsyncOps_ == 0` term in `run()`'s exit condition is therefore
always true — every existing script's behavior, and the full
`forge --self-test` suite, should be bit-for-bit unchanged. This is
deliberately used as the milestone's own verification story instead of
writing a synthetic "manually toggle the counter" test that would just
have to be deleted again the moment 7.5.3 gives the counter a genuine
caller: a real `python mach build` succeeding, plus `forge --self-test`
still printing `[self-test] ALL PASSED` across all 29 cases unchanged,
is sufficient to confirm this addition compiles cleanly and introduces
no regression.

## Phase 7.5.2 real-build-confirmed (2026-08-02)

The user ran `python mach build` (successful) and `forge --self-test`
(`[self-test] ALL PASSED`, all 29 cases, unchanged from before this
milestone) — exactly the "provably inert" outcome this milestone's own
entry predicted, since nothing calls `OnAsyncOperationStarted()` yet.

**Status: Phase 7.5.2 — the `Runtime` pending-operation counter and
`run()`'s event-loop lifetime fix — is real-build-confirmed.** 7.5.3 (the
per-operation bookkeeping struct, the first native `JS::Promise`
creation/resolution, and `fs.readFile` implemented end to end as the
first proof-of-concept method) can start.

## Phase 7.5.3: `fs.readFile` implemented, first native `JS::Promise` use in this codebase (2026-08-02)

Since nothing in this codebase had ever called `JS::NewPromiseObject`/
`ResolvePromise`/`RejectPromise` before, and this project's own standard
(set by Phase 7.2's `JS_NewPlainObject` precedent) is to read the real
declaration rather than reconstruct one from memory or training data,
the actual `js/public/Promise.h` was fetched directly from
`mozilla/gecko-dev` on GitHub — the same upstream repository
`C:\spidermonkey-dev\gecko-dev` is cloned from — before writing any code
against it. Confirmed signatures:

```
extern JS_PUBLIC_API JSObject* NewPromiseObject(JSContext* cx,
                                                 JS::HandleObject executor);
extern JS_PUBLIC_API bool ResolvePromise(JSContext* cx,
                                          JS::HandleObject promiseObj,
                                          JS::HandleValue resolutionValue);
extern JS_PUBLIC_API bool RejectPromise(JSContext* cx,
                                         JS::HandleObject promiseObj,
                                         JS::HandleValue rejectionValue);
```

A null `executor` (used here) means the Promise can only be settled via
`JS::ResolvePromise`/`RejectPromise` — exactly this binding's shape, and
exactly what `FsAsync.md`'s design called for.

**Implementation, following the frozen design closely:**

- `AsyncFsReadOp` (the per-operation bookkeeping struct) is defined in
  `forge.cpp` *before* `Runtime`, specifically so `Runtime`'s own
  `pendingFsReadOps_` member can hold it as a complete type directly.
  Forward-declaring it instead and having `Runtime` hold a
  `HashMap<AsyncFsReadOp*, UniquePtr<AsyncFsReadOp>>` member of an
  incomplete type would raise a real question about exactly when
  `UniquePtr<AsyncFsReadOp>`'s destructor gets instantiated relative to
  `Runtime`'s own implicit destructor (the classic "pImpl needs an
  out-of-line destructor" C++ subtlety) — answerable with a real
  compiler, but not with certainty in this sandbox, so the whole question
  was sidestepped by simply not forward-declaring in the first place.
- `Runtime` gains a shared `ThreadPool workerPool_` (4 workers — sized
  the same as `ThreadingSmokeTest.cpp`'s own real-machine-confirmed case,
  an unvalidated starting point, not a tuned value; real tuning is
  7.5.6), the `pendingFsReadOps_` map (keyed by the op's own address —
  `FsAsync.md` explicitly noted no ID-based lookup is needed here, unlike
  `JsTimerRegistry`'s int-keyed map for `clearTimeout`), and a
  `TraceRoots(trc)` method. `JS_AddExtraGCRootsTracer` now passes `this`
  instead of `&timers_`, so one method traces both the timer registry's
  callbacks and every in-flight read's `Promise` — closing exactly the
  "GC-rooted independent of script" Design Goal `FsAsync.md` called for.
- `fs.readFile(path, encoding?)` parses arguments identically to
  `readFileSync` (Phase 7.3), creates the Promise, registers the
  bookkeeping struct, then submits a worker lambda that captures *only* a
  raw `AsyncFsReadOp*` (touching just its `path`/`wantUtf8`/
  `bytesResult`/`textResult` fields — all forge-core types) and a raw
  `IoLoop*` (only to call `PostCompletion`, real-machine-confirmed by
  7.5.1) — never `cx`, never `op->promise`, never anything JSAPI-typed.
  This is the "thread boundary is absolute" Design Goal, enforced by
  construction rather than by convention alone: the worker's closure
  simply has no way to reach anything JSAPI-typed even by mistake.
- `FsAsyncReadOpCompleted` (the `IoCompletion` callback, dispatched by
  `RunOnce()` on the main thread once the worker's `PostCompletion` fires)
  resolves or rejects the Promise from the worker's stored `Result<T>`.
  Rejection reuses `ThrowJsError` directly rather than duplicating its
  `.code`/`.syscall`/`.path`/`.nativeCode` shape-construction logic: a new
  small `RejectAsyncFsPromise` helper calls `ThrowJsError` (which throws,
  setting cx's pending exception), retrieves that exception as a value,
  clears it (a Promise rejection value must never simultaneously be a
  pending exception on `cx`), and hands it to `JS::RejectPromise`.

**Two known, accepted gaps, disclosed rather than silently assumed
away** (per this project's own "Be Honest" standard):

1. If the worker's own `PostCompletion` call itself fails (the
   completion port somehow invalid), there is no way to report that from
   the worker thread — the op's Promise simply never settles. An
   exceptional, not routine, failure mode; not solved by this milestone.
2. If `run()` exits early (a genuine I/O error elsewhere) while a read is
   still in flight, `Runtime::Shutdown()` was extended to safely drop
   that op's unsettled Promise (via `pendingFsReadOps_.Clear()`, called
   right after `workerPool_.Shutdown()` blocks until every in-flight
   worker has actually finished running — guaranteeing no worker thread
   is still writing to the struct when it's freed — and, critically,
   while `cx` is still alive, so releasing each op's `JS::Heap<promise>`
   happens in a context where its own GC-barrier machinery still has a
   live context to work with, rather than deferring that release to
   `Runtime`'s own implicit destructor, which runs later in `main()`,
   after `JS_DestroyContext(cx)` has already run). This avoids a real
   use-after-cx-destroyed risk, but the dropped operation's actual result
   (if it ever would have completed) is simply lost — harmless, since no
   script is left running to observe it either way, but lossy, and not
   fixed further here.

`manual-test-fs-async.js` (new file) exercises both encodings' success
paths and the `NotFound` rejection path, and — since its only pending
work once its body finishes running is these three `fs.readFile()`
calls, with no timer anywhere in the script — also directly exercises
the Phase 7.5.2 event-loop lifetime fix: without `pendingAsyncOps_`,
`run()` would exit before any of these reads completed. This is
explicitly a stand-in for Phase 7.5.5's real self-test harness (today's
`--self-test` suites call binding functions directly and never let
`Runtime::run()` actually drive the event loop, so they structurally
cannot exercise a real async completion yet), not a replacement for it.

**Status: implemented, pending the user's real `python mach build` and a
real run of `manual-test-fs-async.js`.**

## Phase 7.5.3 real-build-confirmed: `fs.readFile` works end to end (2026-08-02)

The user ran `python mach build` (successful) and
`manual-test-fs-async.js`:

```
Script body finished. If the Phase 7.5.2 event-loop lifetime fix
works, the process now waits for the three reads above to
complete (and prints their PASS/FAIL lines) instead of exiting
immediately.

PASS: fs.readFile rejected with .code === "NotFound" for a missing file
PASS: fs.readFile(path, "utf8") resolved with the correct text
PASS: fs.readFile(path) resolved with a Uint8Array of the right length
```

All three assertions passed, and — notably — the "Script body finished"
line printed *before* the three PASS lines, confirming the process
genuinely kept running after the synchronous script body completed,
waiting for the three in-flight `fs.readFile()` Promises to settle,
instead of exiting immediately. That's real, positive confirmation of
the Phase 7.5.2 event-loop lifetime fix actually doing its job against a
genuine async operation, not just the "provably inert" argument that
milestone's own verification rested on at the time.

This also confirms, together: the first native `JS::NewPromiseObject`/
`ResolvePromise`/`RejectPromise` calls in this codebase work correctly
on both the resolve and reject paths; both `readFile` encodings marshal
correctly from a worker-thread `Result<T>` into the right JS value
(`utf8` string via `FromForgeString`, default `Uint8Array` via
`Uint8ArrayFromBytes`); the rejection shape (`.code === "NotFound"`)
round-trips correctly through `RejectAsyncFsPromise`'s reuse of
`ThrowJsError`; and the worker-thread dispatch via the new shared
`ThreadPool`, the cross-thread `PostCompletion` handoff (7.5.1), and the
`Runtime`-owned `pendingFsReadOps_` GC-rooting all worked together
without incident on first real build -- no bugs surfaced in this pass,
unlike Phase 4/5's resumption, which is a reasonable outcome given how
much more sandbox-side reasoning (real header fetch for `Promise.h`,
careful tracing through `HashMap`/`Result<T>`/`UniquePtr` conventions
already established elsewhere in this file) went into this milestone
before ever reaching the user.

**Status: Phase 7.5.3 -- worker dispatch, native `Promise` settlement,
and `fs.readFile` end to end -- is real-build-confirmed.** 7.5.4 (the
remaining six methods: `writeFile`/`appendFile`/`exists`/`mkdir`/`rm`/
`stat`) can start.

## Phase 7.5.4: remaining six async fs methods implemented (2026-08-02)

Rather than repeating `AsyncFsReadOp`'s whole struct-plus-dispatch shape
six more times, `writeFile`/`appendFile`/`mkdir`/`rm`/`exists`/`stat`
share one new struct, `AsyncFsMiscOp`, discriminated by a `kind` enum --
generalizing the exact pattern `AsyncFsReadOp`'s own
`bytesResult`/`textResult` already established (multiple
placeholder-initialized `Result<T>` fields, exactly one authoritative per
instance, selected by a discriminant) rather than introducing a new
pattern (e.g. an inheritance-based reinterpret_cast trick, which would
have worked but adds a kind of risk this project has no real compiler on
hand to verify, and which nothing else in this codebase does). `Runtime`
gained a second pending-op `HashMap` (`pendingFsMiscOps_`) alongside
7.5.3's read-op map; `TraceRoots`/`Shutdown` were extended to walk both.

Two new small helpers: `WriteAllBytesNoThrow` (a worker-safe twin of the
existing `WriteAllBytes` -- same short-write-retry loop, returns
`Result<void>` instead of calling `ThrowJsError`, since the worker thread
must never touch `cx`) and `CopySpanToVector` (copies `ResolveWriteData`'s
resolved span into an owned `Vector<u8>` before dispatch -- the async
write/append path's first real exercise of `FsAsync.md`'s
copy-before-dispatch rule: the sync path's span may alias the caller's
JS buffer or `ResolveWriteData`'s own `ownedText` storage, neither safe
to hand across the thread boundary to a worker running at some later,
arbitrary time).

`mkdir`/`rm`/`exists`/`stat` each reuse their sync counterpart's exact
`File::*` call, argument parsing, and semantics verbatim (including
`mkdir`'s `options.recursive` split and `exists`'s documented Node
divergence -- a genuine `File::Exists` failure still rejects rather than
resolving `false`), just dispatched through the shared worker path
(`DispatchAsyncFsMiscOp`) instead of running inline.

`manual-test-fs-async.js` (extended from 7.5.3's version, not a new
file) now chains all six new methods sequentially through `.then()` --
`writeFile → appendFile → stat → exists → rm → exists → mkdir
(recursive) → mkdir (non-recursive, expects AlreadyExists rejection)` --
rather than firing them concurrently. This is a deliberate test-design
choice, not an oversight: two of these calls racing over the same path
is explicitly out of scope by `FsAsync.md`'s own Non-Goals (no ordering
guarantee across concurrent calls), so a sequential chain is what makes
this a meaningful, repeatable check rather than exercising undefined
interleaving that could pass or fail depending on which worker thread
happens to finish first.

**Status: implemented, pending the user's real `python mach build` and a
real run of the extended `manual-test-fs-async.js`.**

## Phase 7.5.4 real-build-confirmed: all seven async fs methods work end to end (2026-08-02)

The user ran `python mach build` (successful) and the extended
`manual-test-fs-async.js` -- all eleven checks passed, ending in
`ALL CHECKS PASSED`, with the chained write→append→stat→exists→rm→
exists→mkdir(recursive)→mkdir(non-recursive, `AlreadyExists`) sequence
completing without an uncaught rejection stopping it partway through.

This is the first real-machine confirmation of: the `AsyncFsMiscOp`
shared-struct-plus-`kind`-discriminant design working correctly across
six different operations and three different result shapes
(`Result<void>`/`Result<bool>`/`Result<u64>`); `CopySpanToVector` and
`WriteAllBytesNoThrow` correctly moving write/append's data across the
thread boundary and performing the short-write retry loop on a worker;
`mkdir`'s `options.recursive` parsing and non-recursive `AlreadyExists`
rejection working identically to `mkdirSync`'s own already-confirmed
behavior, just through the async path; and two pending-op `HashMap`s
(`pendingFsReadOps_` and `pendingFsMiscOps_`) being traced and shut down
correctly together, with operations from both in flight simultaneously
during the test run (the three independent `fs.readFile` calls from
7.5.3 ran concurrently with the 7.5.4 chain).

No bugs surfaced in this pass, matching 7.5.3's outcome -- reasonable
given how much of this milestone was a direct, deliberate generalization
of 7.5.3's already-proven pattern rather than new, untested territory.

**Status: Phase 7.5.4 is real-build-confirmed. All seven `fs.*` async
bindings specified in `FsAsync.md`'s Public API
(`readFile`/`writeFile`/`appendFile`/`exists`/`mkdir`/`rm`/`stat`) now
exist, work correctly, and are confirmed on the real machine.** 7.5.5
(the formal Promise-based self-test harness, replacing
`manual-test-fs-async.js` as this feature's ongoing regression check) is
next.

## Phase 7.5.5: formal Promise-based self-test harness implemented (2026-08-02)

Added `RunFsAsyncSmokeTests(cx, runtime)` to `forge.cpp`, called from
`main()`'s `--self-test` dispatch after `RunMarshallingSmokeTests` and
`RunFsSmokeTests`. This is the first self-test suite in this file that
calls `Runtime::run()` at all -- every prior suite (Phase 7.2/7.3) calls
a binding function directly and inspects a `Result<T>`/thrown exception
synchronously, since nothing before Phase 7.5 had anything asynchronous
to wait on.

Two groups, matching FsAsync.md's own Acceptance Criteria:

- Group A evaluates one script exercising all seven `fs.*` async
  methods (the three `fs.readFile` cases concurrently, the six Phase
  7.5.4 methods chained through `.then()`, identical structure to
  `manual-test-fs-async.js`), then calls `runtime.run()` once to drive
  every chain to completion. Each of the 12 expected cases records its
  own pass/fail via a new native `globalThis.__asyncSelfTestRecord(name,
  passed, message)` callback (bound only for this suite's duration, not
  part of `Fs.md`'s JS-visible surface) into a `Vector<AsyncSelfTestResult>`
  -- the only way to observe an async completion from native code, since
  it can't itself await a JS `Promise`. The harness also checks that
  exactly 12 results were recorded, not just that none of them failed,
  so a chain that throws partway through and silently stops doesn't look
  identical to "zero failures."
- Group B evaluates a second, separate script whose *only* statement is
  one `fs.readFile(...).then(...)` call -- no timers, nothing else
  queued -- and calls `runtime.run()` a second time on the same
  `Runtime` instance. This is the direct, isolated regression test
  FsAsync.md's Acceptance Criteria specifically calls for: confirming
  the Phase 7.5.2 `pendingAsyncOps_` fix, in a script shaped so that
  without it, `run()` would see an empty job queue and an empty `IoLoop`
  and return immediately, and the `.then()` callback would never fire.
  Group A's chain always has other pending work alongside each
  individual async call, so it would not by itself catch a regression
  here -- this probe exists specifically because Group A can't cover it.

Calling `runtime.run()` twice in sequence on the same `Runtime` (once
per group) is a new usage pattern -- every prior caller (`main()`'s own
script-running path) calls it exactly once per process. Reasoned through
rather than assumed safe: Group A's own `run()` loop only returns once
`jobQueue_.empty() && loop_.Empty() && pendingAsyncOps_ == 0`, so by the
time Group B's script is evaluated, the `Runtime` is back in the same
idle state it was in right after `Initialize()` -- nothing carries over
except the still-alive `IoLoop`/`ThreadPool`, which is exactly what
Group B's probe needs alive to dispatch its own single read. No change
was made to `run()`, `Initialize()`, or `Shutdown()` themselves for this.

**Status: implemented, not yet real-build-confirmed.** This sandbox has
no real compiler available -- only a brace/paren-balance check was
possible here (see AGENTS.md's "Be Honest" principle: this is explicitly
not claimed as verified). Needs a `python mach build` and a
`forge --self-test` run on the real machine, expected to print 13 new
`[self-test]` lines on success (12 from Group A plus 1 from Group B --
the result-count check only prints its own line if the count is *not*
12) in addition to the existing Phase 7.2/7.3 suites' output,
before this milestone can be marked done.

## Phase 7.5.5 real-build-confirmed: Promise-based self-test harness works end to end (2026-08-02)

The user ran `python mach build` (successful) and `forge --self-test`:
all 13 expected new lines appeared, all passing, ending in
`ALL PASSED`:

```
[self-test] fs.readFile rejects with .code === "NotFound" for a missing file PASS
[self-test] fs.readFile(path, "utf8") resolves with correct text PASS
[self-test] fs.readFile(path) resolves with a Uint8Array of the right length PASS
[self-test] fs.writeFile resolves and writes the expected content PASS
[self-test] fs.appendFile resolves and appends the expected content PASS
[self-test] fs.stat resolves with the correct size PASS
[self-test] fs.exists resolves true for a file that exists PASS
[self-test] fs.rm resolves and the file is actually gone PASS
[self-test] fs.exists resolves false for a file that no longer exists PASS
[self-test] fs.mkdir(recursive) resolves and the nested directory actually exists PASS
[self-test] fs.mkdir(non-recursive) on an existing directory rejects with AlreadyExists PASS
[self-test] fs.* chained misc sequence ran to completion without an uncaught rejection PASS
[self-test] Runtime::run() waits for lone in-flight fs.readFile (no timers) PASS
```

No count-mismatch line appeared, confirming Group A's chain recorded
exactly the 12 cases expected rather than silently stopping partway
through. This is the first real-machine confirmation of: calling
`Runtime::run()` a second time on the same `Runtime` instance (Group A's
cycle, then Group B's) working correctly -- the loop genuinely returned
to an idle state after Group A and then correctly blocked again for
Group B's lone in-flight read, rather than misbehaving on a second
entry; the native `globalThis.__asyncSelfTestRecord` recording callback
round-tripping name/passed/message correctly across all 13 calls; and,
specifically via Group B, the Phase 7.5.2 `pendingAsyncOps_` fix holding
under the exact isolated shape (one in-flight async fs call, zero
timers, zero other queued work) FsAsync.md's own Acceptance Criteria
called out as a required, dedicated regression case -- not merely
inferred from Group A's busier chain, which always has other pending
work alongside each individual call and so could not have caught a
regression here on its own.

No bugs surfaced in this pass -- the fourth consecutive Phase 7.5
milestone (after 7.5.2, 7.5.3, 7.5.4) to pass real-build verification on
the first attempt.

**Status: Phase 7.5.5 is real-build-confirmed. `forge --self-test` now
has a genuine Promise-based self-test harness that drives
`Runtime::run()` to completion and inspects real async fs.* outcomes
afterward, replacing `manual-test-fs-async.js` as this feature's ongoing
regression check going forward.** Only 7.5.6 (benchmarks + docs) remains
before Phase 7.5 (async fs) is complete end to end.

## Phase 7.5.6: async fs benchmark drafted (2026-08-02)

Added `bench/fs-async-bench.js`, the Promise-returning counterpart to
`fs-bench.js` (Phase 7.4) -- same iteration counts (2000 round-trips, 100
"touch every remaining method" passes) and payload, for a direct,
apples-to-apples comparison, but chained through `.then()` one step at a
time rather than `fs-bench.js`'s blocking for-loop, since FsAsync.md's
own Non-Goals rules out any ordering guarantee across concurrent async fs
calls and this benchmark's round-trip pass reuses one shared file across
every iteration -- firing all 2000 writes/reads at that path concurrently
would be a genuine race, not a fair throughput measurement. Also updated
`run-benchmarks.ps1` to include it in `$Benchmarks` and documented the
addition in that script's own header comment, matching the convention
every prior benchmark addition (Phase 6, Phase 7.4) followed.

This sandbox has no real compiler to build Forge itself, so the script
obviously couldn't be run against a real `forge.exe` here -- but unlike
every prior benchmark script added in this project, this one *could* be,
and was, actually executed against the two other runtimes it's meant to
compare against: both `node` (v22.22.2) and `bun` (v1.3.13) are present
in this sandbox. Running it caught a real, genuine bug before delivery:
Node's (and Bun's) `fs.promises` has no `.exists` method at all -- Node
deprecated the callback-based `fs.exists` outright and never added a
Promise-based replacement, keeping only the synchronous
`fs.existsSync` -- so the script's first draft failed immediately with
`fs.exists is not a function`. Fixed by shimming `fs.exists` on top of
`fs.promises.access()` (resolves on success, rejects on failure) when
running under Node/Bun, normalized to the same resolves-true/
resolves-false shape Forge's own `fs.exists` uses. After the fix, both
`node fs-async-bench.js` and `bun fs-async-bench.js` ran to completion
and printed matching output:
`fs-async-bench done, iterations=2000, extraIterations=100, checksum=721300`
-- the identical checksum across both runtimes is itself a useful
cross-check that the shim makes both do exactly the same work, not just
"run without crashing."

Also updated `FsAsync.md`'s own `Implementation Status` section (stale
since design-freeze -- it still said "Design only. Nothing in this
document is implemented yet." despite 7.5.1-7.5.5 all being
real-build-confirmed by this point) with a full milestone-by-milestone
breakdown, and made light-touch updates to `Fs.md` and `JsBindings.md`'s
few forward-references to "Phase 7.5" (Non-Goals, Thread Safety,
Extensibility, the `*Sync`-suffix-reservation note) to point at
`FsAsync.md`'s now-real-build-confirmed status instead of describing it
as future work -- neither doc's own frozen content (its actual specified
behavior) was changed, only these pointers.

**Status: `fs-async-bench.js` is written and confirmed runnable (via
Node/Bun, both matching) but not yet run against a real `forge.exe` --
that requires the user's real machine.** Once real numbers come back,
`Forge_Benchmark_Report.md` gets a new results section/analysis (same
format as its existing `fs-bench.js` entry) and `PROJECT_CONTEXT.md`'s
Phase 7.5 bullet gets updated to mark 7.5.6, and therefore the whole of
Phase 7.5, done.

## Phase 7.5.6 benchmark run: six scripts reconfirmed, fs-async-bench.js failed on Forge (2026-08-02)

The user ran `run-benchmarks.ps1` for real. The seven pre-existing
scripts (`startup.js` through `fs-bench.js`) all landed within a few
percent of the 2026-07-30 baseline in `Forge_Benchmark_Report.md` --
noise, not a real change, consistent with nothing in Phase 7.5 touching
any of those code paths.

`fs-async-bench.js` itself did not produce a usable Forge number.
Bun (median 1,237.52 ms) and Node (median 1,394.69 ms) both ran it to
completion. Forge failed outright:

```
fs-async-bench.js        forge  FAILED (exit 1): C:\spidermonkey-dev\gecko-dev\js\src\forge\bench\fs-async-bench.js:138:4 fs.mkdir is not a function
```

Line 138 is this script's very first async fs.* call -- the top-level
`fs.mkdir(SCRATCH_DIR, { recursive: true })` that kicks off the whole
chain, before any `fs.readFile`/`fs.writeFile`/etc. ever runs. Being a
synchronous `TypeError` on a top-level (non-callback) call, this failed
`JS::Evaluate` itself rather than surfacing as a rejected Promise --
consistent with `run-benchmarks.ps1` reporting a hard exit code 1, not a
script that ran and printed a FAILED line of its own.

**This is a genuinely puzzling result, not a straightforward "the
feature is broken" -- flagged as an open, unresolved discrepancy rather
than guessed at.** The very same real machine's `forge --self-test` run,
reported in this same conversation just before this benchmark run,
explicitly showed `fs.mkdir` passing twice (`fs.mkdir(recursive)
resolves and the nested directory actually exists` and `fs.mkdir
(non-recursive) on an existing directory rejects with AlreadyExists`,
both `PASS`, among all 13 Phase 7.5.5 self-test lines). `forge.cpp`'s
`DefineFsNamespace` registers `mkdir` (and every other async method)
unconditionally -- the exact same call handles both `--self-test` and
any ordinary script; there is no code path where one gets `fs.mkdir` and
the other doesn't. That symmetry, plus the self-test having *just*
passed, points away from a code regression in `forge.cpp` and toward an
environment/binary discrepancy: **the leading hypothesis is that
`C:\Forge\bin\forge.exe` -- the path `run-benchmarks.ps1` defaults to,
and the only Forge binary this run used -- is a different, older copy
than whatever binary the `--self-test` run actually executed.** Not
confirmed -- this depends on the user's own build/deploy workflow
(whether `mach build`'s output gets copied to `C:\Forge\bin\forge.exe`
every time, or only sometimes), which is outside what this project's
own files can answer on their own.

Since only `mkdir` was ever reached before the failure, whether the
*other* six async methods would also be missing from whatever binary
`run-benchmarks.ps1` used is genuinely unknown -- not assumed either
way.

**Status: Phase 7.5.6 is NOT done.** `Forge_Benchmark_Report.md` has been
updated with the six reconfirmed scripts' numbers and `fs-async-bench.js`'s
partial (Bun/Node only) results, with this discrepancy flagged prominently
rather than papered over. `ROADMAP.md`/`PROJECT_CONTEXT.md` are not being
marked done for 7.5.6 or Phase 7.5 as a whole until this is resolved --
either the binary gets refreshed and the benchmark reruns cleanly, or a
real bug is found and fixed.

## Phase 7.5.6 resolved: stale binary confirmed, clean benchmark run, Phase 7.5 complete (2026-08-02)

The stale-binary hypothesis from the entry above was correct, confirmed
within minutes by the user:

```
Copy-Item "C:\spidermonkey-dev\gecko-dev\obj-spider\dist\bin\forge.exe" "C:\Forge\bin\forge.exe" -Force
```

`C:\Forge\bin\forge.exe` had not been refreshed from `mach build`'s
actual output since before Phase 7.5's async `fs.*` work landed --
whatever binary `forge --self-test` ran against (presumably `mach
build`'s own output directory, run directly) was current; the separate,
manually-copied `C:\Forge\bin\forge.exe` `run-benchmarks.ps1` defaults to
was not. No code in `forge.cpp` needed to change. This is a real, useful
process finding for this project going forward, not just a one-off
annoyance: **refresh `C:\Forge\bin\forge.exe` from the `mach build`
output before every benchmark run**, not only after a build that just
added a feature.

After the copy, a clean rerun of `run-benchmarks.ps1` produced real
Forge numbers for `fs-async-bench.js` for the first time:

```
fs-async-bench.js        forge  avg=    944.99 ms   median=    935.39 ms   min=    913.91 ms
fs-async-bench.js        bun    avg=  1,236.58 ms   median=  1,239.99 ms   min=  1,181.34 ms
fs-async-bench.js        node   avg=  1,482.14 ms   median=  1,413.65 ms   min=  1,392.17 ms
fs-async-bench.js        forge/bun  ratio = 0.75x
fs-async-bench.js        forge/node ratio = 0.66x
```

Forge is faster than both Bun and Node on the async fs.* round-trip
benchmark, directionally consistent with `fs-bench.js`'s own sync-path
result (0.76x/0.86x) rather than reversing which runtime wins. All three
runtimes are slower on the async version than the sync one for the same
amount of I/O work, as expected -- Promise creation/settlement overhead,
and for Forge specifically a real cross-thread `ThreadPool` hop per
operation, on top of the underlying I/O. The other seven scripts all
landed within a few percent of the 2026-07-30 baseline, confirming
nothing in Phase 7.5 regressed anything measured before it.

`Forge_Benchmark_Report.md` has been updated with the full results table
(all eight scripts), a `fs-async-bench.js`-specific analysis paragraph,
and the stale-binary discrepancy's resolution -- the earlier "flagged as
open, unresolved" language is marked resolved rather than deleted, so the
report's own history stays honest about what actually happened.
`ROADMAP.md`'s 7.5.6 entry and `PROJECT_CONTEXT.md`'s Phase 7.5 bullet
are both updated to reflect the whole phase as done.

**Status: Phase 7.5.6 is done. Phase 7.5 (async `fs`) is complete
end to end** -- all seven `fs.*` async methods implemented,
real-build-confirmed individually (7.5.1-7.5.4), covered by a formal
Promise-based self-test harness (7.5.5), and now benchmarked against
Bun/Node with real numbers (7.5.6). No known open items remain for this
phase.

## Phase 7.9: foundation fixes (2026-11-09)

The first phase of Master Plan v2. No new features: it fixes known bugs
from the research studies and puts safe defaults in place before
modules and Kavach are built on top. Every change is marked `Phase 7.9`
in the code.

**Runtime correctness (`forge.cpp`)**

- **Microtasks are popped before they run.** A microtask that threw was
  never removed from the queue, so it ran again on every drain. Each job
  now also runs in its own function's realm (`JSAutoRealm`), and the queue
  calls `JS::JobQueueIsEmpty` / `JobQueueMayNotBeEmpty` and
  `JS::ClearKeptObjects` exactly like SpiderMonkey's own
  `InternalJobQueue` (js/src/vm/JSContext.cpp).
- **Uncaught errors are fatal, with a stack trace.** An exception in the
  main script, a timer callback or a microtask prints the error plus its
  stack and the process exits with code 1. Nothing runs after it. Before,
  the process exited 0.
- **Unhandled Promise rejections exit 1** (`JS::SetPromiseRejectionTrackerCallback`).
  They are reported once the microtask queue drains, like Node since v15.
- **`setTimeout(fn)` works.** The delay was required; calling without it
  returned `false` with no exception, which SpiderMonkey treats as an
  uncatchable termination, so the script silently died. The same bug hit
  `clearTimeout()` and `queueMicrotask()` with missing arguments. Missing
  delays now default to 0, non-callable callbacks throw a catchable
  `TypeError`, delays above 2^31-1 ms become 1 ms (Node), and the
  undefined-behaviour double-to-uint64 cast is gone.
- Any microtasks still queued at shutdown are released while the
  context is alive (the queue is a static object).

**Safe defaults**

- **SharedArrayBuffer is off** (Spectre risk, study 15).
- **Code-generation hook installed** (`JS_SetSecurityCallbacks`): every
  `eval`, `new Function` and WebAssembly compilation now goes through
  `ForgeCodeGenerationAllowed`. New flags
  `--disallow-code-generation-from-strings` and `--disallow-wasm`.
- **Real GC settings** instead of the 512 MB *hard* cap
  (`JS_NewContext`'s argument is `JSGC_MAX_BYTES`, a hard limit, not a
  trigger). The heap is uncapped by default (`--max-heap-mb=N` sets one),
  and GC heuristics are sized to physical memory.
- **Async stack capture is off by default** (debuggee-only), the likely
  cause of slow Promise steps (study 10). `--async-stacks` turns it back
  on. **Needs an A/B benchmark run on real hardware before it is final.**
- **DLL search hardening** at startup (`forge-core/platform/Process.cpp`):
  only System32 and forge.exe's folder, never the current directory.

**forge-core memory safety (rule R10)**

- `FORGE_ASSERT` is now a release fast-fail (`__fastfail` on Windows).
  The mach build defines `NDEBUG`, so every assert used to be compiled
  out. New `FORGE_RELEASE_ASSERT` / `FORGE_DEBUG_ASSERT`.
- Bounds checks on `operator[]`, `Front`, `Back` in `Vector`, `Queue`,
  `String`, `Span` (so `StringView`) and `Array`.
- **Checked size math** (`CheckedMath.h`) in `Vector`, `Queue`, `String`
  and `HashMap`. This fixes the heap overflow study 13 reproduced with
  ASan (`Vector<u64>::Reserve(SIZE_MAX/8 + 3)`). New `OverflowTest.cpp`.
- **`Path::Normalize` rewritten** around a real root parser: UNC shares
  (`//server/share`), device paths (`//?/C:`, `//?/UNC/...`, `//./PIPE`)
  and drive-relative paths (`C:foo`) keep their root, and `..` can no
  longer climb out of a share. A property fuzzer also found that
  `./C:/x` normalized to the absolute `C:/x`; fixed (a leading `./` is
  kept). New table in `PathTest.cpp`.
- `File.cpp`: paths longer than `INT_MAX` bytes are rejected instead of
  truncated, and reads/writes above 4 GiB are chunked instead of
  silently truncated to a `DWORD`.

**Sockets**

- `SO_REUSEADDR` replaced with `SO_EXCLUSIVEADDRUSE`. On Windows,
  `SO_REUSEADDR` lets another process bind the same port and steal
  connections.
- Sockets are created non-inheritable (`WSA_FLAG_NO_HANDLE_INHERIT`).
- New `Socket::Accept(Endpoint& peer)` returns the peer address
  (IPv4-mapped addresses unwrapped). Plain `Accept()` is unchanged.

**Tests and CI**

- `forge --self-test` has a new runtime-safety suite (timers,
  queueMicrotask, SharedArrayBuffer, the code-generation hook).
- `tests/run-runtime-tests.ps1` runs forge.exe on small scripts and
  checks exit codes and output (uncaught errors, rejections, event order).
- `.github/workflows/ci.yml` with `tools/ci/forge-core-tests.sh` (Linux,
  ASan + UBSan) and `tools/ci/forge-core-tests.ps1` (Windows, MSVC). A CI
  job that builds forge.exe itself comes with Phase 7.95.
- `SECURITY.md` added.

**Verification (honest)**

- forge-core portable tests: **passed** under g++ 11 with ASan + UBSan
  (Linux sandbox), including the new OverflowTest and the Path table. A
  300,000-input Path property fuzz found one bug (fixed above), then 0.
- Windows forge-core files (`File.cpp`, `Socket.cpp`, `Process.cpp`,
  `IocpLoop.cpp`, threads) and every Windows smoke test: **compile and
  link with MinGW g++ 13**, no warnings with `-Wall -Wextra`. Not run.
- `forge.cpp`: **type-checks cleanly with clang 18 against this tree's
  real SpiderMonkey headers** (`obj-spider/dist/include`, MinGW target).
  This is not the real clang-cl `mach build`, and nothing has run yet.
- **Still needed on the real machine:** `python mach build`, then
  `forge --self-test`, `tests\run-runtime-tests.ps1`, the forge-core
  smoke tests, and the benchmarks with and without `--async-stacks`.

**Not done in this phase (moved on):** benchmarks still include startup
time; install-folder ACLs; Unicode console and `wmain` (Phase 8).

## Phase 7.9 real-build-confirmed (2026-11-09)

Varad ran the real build on Windows: `python mach build` succeeded (no
warnings from Forge's own files; mach needed Python 3.12, not conda's 3.14).
Results with the freshly copied `C:\Forge\bin\forge.exe`:

- `forge --self-test`: **ALL PASSED** (12 marshalling, 6 new
  runtime-safety, 17 fs sync, 13 fs async cases).
- `tests\run-runtime-tests.ps1`: **ALL PASSED** (9 scripts: clean exit,
  `setTimeout(fn)`, event order, handled / unhandled / late-handled
  rejections, uncaught errors in the script, a timer and a microtask).
- `tools\ci\forge-core-tests.ps1` (MSVC, C++17, no exceptions): **ALL
  PASSED** (17 tests, including the File, Socket, threading, timer and IOCP
  smoke tests, OverflowTest and the new Path table).

Still open: the `--async-stacks` A/B benchmark. Note also: `forge.exe`
needs `mozglue.dll` next to it; `CLAUDE.md` now documents the copy step.
(Corrected 2026-11-10: this entry first said `build.bat`/`install.ps1` do
not exist. They do, in `js/src/forge`, not in the tree root where they
were looked for; they copied `forge.exe` only, not `mozglue.dll`.)

## Phase 7.9 follow-up: async-stacks A/B (2026-11-09)

One run each of `bench\promise-chain-bench.js` on Varad's machine:
**63 ms** with the new default (async stacks captured only for debuggees)
vs **96 ms** with `--async-stacks`. Including about 20 ms of startup, so the
Promise work itself takes roughly 45% less time. The default stays. A full
7-iteration `run-benchmarks.ps1` run should replace these single samples in
`Forge_Benchmark_Report.md`.

## Phase 7.95: moving to SpiderMonkey ESR 153 (started 2026-11-09)

**Engine facts (verified):** Firefox 153.0esr shipped in July 2026; the
`esr153` branch of github.com/mozilla-firefox/firefox is at 153.8.0esr,
commit `c318e3511672a445f5e218f7c6360189de329924`, now pinned in
`engine/engine.toml`. Mozilla supports it until 2027-09-21; ESR 175 follows.

**What changed between 142 Nightly and ESR 153 for Forge** (found by
type-checking forge.cpp against ESR 153's real headers):

1. **The engine owns the microtask queue now** (`js/friend/MicroTask.h`).
   `JS::JobQueue` lost `empty()` and `enqueuePromiseJob()`, and
   `getHostDefinedData()` gained an incumbent-global parameter. Promise jobs
   are engine "JS microtasks" run with `JS::RunJSMicroTask` in the realm of
   `JS::GetExecutionGlobalFromJSMicroTask`; embedders enqueue their own jobs
   with `JS::EnqueueMicroTask`. forge.cpp now has both job queues, selected
   by `__has_include("js/friend/MicroTask.h")`
   (`FORGE_ENGINE_OWNS_MICROTASK_QUEUE`), so the same file builds on 142 and
   on ESR 153. The drain policy is identical on both.
2. **ESR 153's headers require C++20** (`std::bit_width`, concepts, `<=>`).
   Forge's code must now compile as both C++17 and C++20;
   `tools/ci/forge-core-tests.sh` builds every forge-core test both ways.

Nothing else in forge.cpp needed to change: the security callbacks,
rejection tracker, ContextOptions, GC parameters and every other JSAPI call
used today are the same in ESR 153.

**Forge in its own repository:** github.com/varadTheDeveloper/forge is the
source of truth. `tools/setup-engine.ps1` fetches the pinned engine commit
(one commit deep), verifies it, copies Forge to `js/src/forge`, adds
`"forge"` to `js/src/moz.build` (the only engine-tree change), and writes a
`.mozconfig`. `VENDORED.toml` records the engine's owner, advisory feed and
72-hour patch deadline (rule R23).

**Verification (honest):**
- forge.cpp type-checks with **zero errors** against ESR 153's headers as
  C++20 (clang 18, MinGW target). The generated config headers came from the
  142 build, with a hand-made stand-in for `js/PrefsGenerated.h`, so this is
  a strong signal, not a build.
- forge.cpp still type-checks cleanly against the 142 headers as C++17.
- forge-core: all portable tests pass as C++17 and C++20 under ASan+UBSan;
  all Windows files compile as C++20 with MinGW, no warnings.
- **Not yet done:** a real `mach build` of ESR 153 with Forge, then
  `--self-test`, the runtime tests and the benchmarks on it. A CI job that
  builds the engine and forge.exe on GitHub Actions comes after the first
  successful local ESR 153 build, so it copies steps that are known to work.

## Phase 7.95 real-build-confirmed on SpiderMonkey ESR 153 (2026-11-09)

Varad ran `tools\setup-engine.ps1`, `mach bootstrap` (SpiderMonkey) and
`mach build` in `C:\forge-engine\esr153`. The build produced
`obj-forge\dist\bin\forge.exe` against **MOZJS 153.8** (engine linked
statically; only `mozglue.dll` is needed next to it). One fix was needed
on the way: ESR 153's build runs SpiderMonkey's include-style checker
(`config/check_spidermonkey_style.py`) over all of `js/src`, and Forge does
not follow SpiderMonkey's include conventions. `setup-engine.ps1` now adds
`js/src/forge/` to the checker's ignore list (the second and last change
Forge makes inside the engine tree). The script also skips the ~1 GB
download when the checkout is already at the pinned commit.

Results with `C:\Forge\esr153\forge.exe`:

- `forge --self-test`: **ALL PASSED** (48 cases). Every Promise, `await`,
  `queueMicrotask` and async fs case now runs through ESR 153's
  engine-owned microtask queue.
- `tests\run-runtime-tests.ps1`: **ALL PASSED** (9 scripts).
- `bench\promise-chain-bench.js`, one run: **42 ms** (63 ms on 142 with
  the same Forge code). For reference only, the last full benchmark report
  measured Bun at 44 ms and Node at 48 ms on this script; a proper
  7-iteration run on ESR 153 is still to do.

Still open for Phase 7.95: a CI job that builds the engine and forge.exe;
deciding when to drop the 142 code path; the full benchmark rerun.

## Benchmark run on ESR 153, 7 iterations (2026-11-10)

Full results and analysis: `Forge_Benchmark_Report.md`, "Results
(2026-11-10)"; raw data in `bench/results/2026-11-10_145510.csv`.

The harness's own ratio table shows Forge ahead of Bun and Node on seven of
eight scripts. **That overstates Forge's throughput.** Every number includes
process startup, and Forge starts 31 ms sooner (17 ms against 48 ms), which
is more than the whole workload of the short scripts. With each runtime's
startup subtracted (an estimate): Forge is ahead on async file I/O and
timers, ties Node on sync file I/O and on the loop benchmark, and is behind
Bun on JSON (1.8x), microtasks (1.8x) and promise chains (3.4x; about
300 ns per step against 88 ns for Bun and 110 ns for Node).

Corrects two earlier statements made on 2026-11-09 from single runs: the
promise-chain times (63 ms, then 42 ms) were compared with Bun's 44 ms and
Node's 48 ms totals, which are almost entirely startup. The improvement
itself is real (work time roughly 69 ms on the old build, 15 ms now), but
Forge has not caught Bun or Node on promise throughput.

Also noted: sync file I/O lost ground against both Bun and Node compared
with the 2026-08-02 run (0.76x to 0.88x against Bun). Unexplained; see the
report's open items. The README's benchmark table was replaced with the
startup-subtracted numbers.

## Phase 7.95: 142 code path removed, one-command build, CI builds forge.exe (2026-11-10)

**The 142 Nightly engine is no longer supported.** `forge.cpp` kept two job
queues since the ESR 153 port, selected with `__has_include`. The old one
(Forge's own `Queue<UniquePtr<Microtask>>`, traced from `TraceForgeRoots`,
for engines that call `enqueuePromiseJob`) is deleted, along with the
`Microtask` struct and the `forge-core/Queue.h` include. The engine-owned
queue (`js/friend/MicroTask.h`) is the only path. About 170 lines removed.

**C++ standard.** The ESR 153 `mach build` compiles every file with
`-std:c++20` (checked in the build's `compile_commands.json`), because the
engine's headers need it. `forge.cpp` is therefore C++20. forge-core includes
no engine headers and still keeps to the C++17-compatible subset (the
Manifesto and decision D5 say C++17); CI builds its tests both ways.

**Source of truth moves out of the old gecko-dev tree.** This repository
(github.com/varadTheDeveloper/forge) is now the source of truth, cloned to
its own folder. `C:\spidermonkey-dev\gecko-dev\js\src\forge` is retired.
New in the repository:

- `tools/build.ps1`: copies the checkout into the engine tree, runs
  `mach build`, installs `forge.exe` and `mozglue.dll` to `C:\Forge\bin`,
  and with `-Test` runs the self-test and the runtime tests. `build.bat` is
  now a one-line wrapper around it, and `install.ps1` copies from the new
  engine location. (Both used to assume the 142 tree's `obj-spider`.)
- `tools/setup-engine.ps1`: sets `core.longpaths` in the engine checkout and
  exits 0 explicitly (robocopy's success codes are non-zero and would have
  failed a CI step).
- `.gitattributes` (`* -text`, the Firefox tree's own rule: store every file
  byte-for-byte) and `.gitignore` (test build output, scratch folders).
- `.github/workflows/ci.yml`: a third job, `forge-windows-engine`, installs
  MozillaBuild 4.2.1 (which mach requires at `C:\mozilla-build`; the
  installer must carry a valid Mozilla Corporation signature), runs
  `setup-engine.ps1`, `mach bootstrap` and `build.ps1 -Test` on a clean
  Windows runner. Runs are skipped for documentation-only pushes, and a
  newer push cancels a run still in progress.

**Verification (honest):** the trimmed `forge.cpp` type-checks with zero
errors against the **real** ESR 153 build headers copied from
`C:\forge-engine\esr153\obj-forge` (clang 18, C++20, MinGW target). It has
not yet been through a real `mach build`; the new CI job is that build. The
PowerShell scripts could not be run in the sandbox, so CI is also their
first real test.

## Phase 7.95 confirmed in CI: forge.exe builds on a clean machine (2026-11-10)

The first run of the new `forge-windows-engine` job passed
(github.com/varadTheDeveloper/forge, Actions run 42808610738, commit
`817a011`). On a clean GitHub-hosted Windows Server 2022 runner it:

1. installed MozillaBuild 4.2.1 (Mozilla Corporation signature valid;
   SHA-256 `8ef5b3d2087e4867dc75bf19f5e62f12e917f518239415723cc70896b6f1e3d8`,
   now pinned in `ci.yml`; its bundled Python is 3.12.10) in 1 min;
2. ran `tools/setup-engine.ps1`: fetched and checked out the pinned ESR
   153.8.0 commit and added Forge to it, 11 min;
3. ran `mach bootstrap` non-interactively, 7 min;
4. ran `tools/build.ps1 -Test`: `mach build`, install, `forge --self-test`
   (**ALL PASSED**) and `tests/run-runtime-tests.ps1` (**9 of 9, ALL
   PASSED**), 17.5 min.

About 37 minutes in total, with no caching yet. This is the first real
build of the `forge.cpp` with the 142 code path removed, and the first real
run of `build.ps1`, so both are now real-build-confirmed. Phase 7.95's exit
criterion in the master plan ("all tests green in CI on ESR 153") is met.

Possible later speed-ups: cache `~/.mozbuild` (toolchains) and the engine
checkout plus `obj-forge` keyed on the pinned commit, so a normal run only
recompiles Forge's own files.

## Phase 7.95 done: building from the new source folder (2026-11-10)

Varad ran `C:\Forge\src\tools\build.ps1 -Test` from the new clone of this
repository. The build recompiled only Forge's own files (17 seconds), then
`forge --self-test` passed all 48 cases and `tests\run-runtime-tests.ps1`
passed all 9. The edit-build-test loop from `C:\Forge\src` works end to end,
on Varad's machine and in CI. `C:\spidermonkey-dev\gecko-dev\js\src\forge`
is retired (see `RETIRED.md` there).

Working agreement: Varad runs builds and tests on his machine when asked;
Claude makes the code changes, commits them under Varad's name and pushes
them to GitHub.

## Benchmarks: work time measured inside each script; report reorganised (2026-11-11)

The 2026-11-10 run showed that the harness's totals are dominated by process
startup on the short scripts. Changes:

- Every `bench/*.js` script now measures its own workload with `Date.now()`
  and prints `work-ms=<n>`. `run-benchmarks.ps1` records that alongside the
  total, compares runtimes on work-time medians, prints startup separately,
  saves the OS, CPU, RAM, power plan and runtime versions to
  `bench/results/<timestamp>.env.txt`, runs from the `bench` folder (so the
  fs scratch folders no longer land in whatever directory it was started
  from), and defaults to 7 iterations.
- Larger workloads where the old ones were a few milliseconds:
  `loop-bench` fib(34) to fib(36), `timer-bench` 5,000 to 100,000 timers,
  `microtask-bench` 200,000 to 1,000,000, `promise-chain-bench` 50,000 to
  1,000,000 steps. Results for these four are not comparable across this
  date.
- `Forge_Benchmark_Report.md`: current results first, a "How the benchmarks
  work" section, and the older runs moved under "History" with a list of the
  conclusions that no longer hold.

All eight scripts were checked under Node 22 in the sandbox (each prints its
`work-ms` line). The PowerShell harness could not be run in the sandbox; its
first run is Varad's.

## First benchmark run with measured work time (2026-11-10, evening)

The new harness ran cleanly on Varad's machine (Ryzen 5 5600X, Windows 11
build 26200; Bun 1.3.14, Node 24.11.1). Full table in
`Forge_Benchmark_Report.md` → "Current results"; raw data in
`bench/results/2026-11-10_190436.csv` and `.env.txt`.

Work time per workload (Forge / Bun / Node, ms): JSON 361 / 209 / 286, loop
131 / 82 / 129, timers 40 / 43 / 35, microtasks 122 / 54 / 254, promise
chains 246 / 60 / 32, sync fs 1,609 / 1,908 / 1,674, async fs 1,683 / 1,946 /
2,050. Startup 15 / 40 / 43.

What changed in our understanding: timers are level, not a Forge win (that
was startup); the promise-chain gap is larger than estimated, 4.1x against
Bun and 7.7x against Node, and is the main throughput target; file I/O and
startup are where Forge leads. File I/O was about 60% slower for all three
runtimes than in the morning run, probably Windows Defender scanning the new
scratch-folder location; the cross-runtime comparison is unaffected. The
README table now shows these measured work times.

## Phase 8.1: console, the Forge namespace, Unicode I/O, Ctrl+C (2026-11-10)

Phase 8 is split into four steps: 8.1 runtime basics (this entry), 8.2 the
Kavach checker, 8.3 the full filesystem API, 8.4 types, fuzzing and bypass
replays.

**Unicode in and out.** `forge.exe` now starts in `wmain` and converts the
UTF-16 command line to UTF-8 (`main` received it in the ANSI code page, so
characters outside it became `?`). Output goes through
`forge-core/platform/Stdio`: `WriteConsoleW` when the stream is a console,
raw UTF-8 `WriteFile` for pipes and files. `print`, `console` and uncaught
error reports all use it. Conversion helpers are in `platform/Unicode`;
invalid input becomes U+FFFD rather than an error.

**`console`** (`runtime/Console.cpp`), written in C++: log, info, debug,
warn, error, dir, dirxml, assert, trace, count, countReset, time, timeLog,
timeEnd, group, groupCollapsed, groupEnd, with the WHATWG `%s %d %i %f %o %O
%c %%` formatter. `console.table` prints like `console.log` for now. Values
are formatted by `runtime/Inspect.cpp` in Node's style. Formatting never runs
script code: getters show as `[Getter]`, Proxies as `[Proxy]`, and no
toString, valueOf or iterator is called (Map and Set entries are read with
the engine's own forEach and a native callback). This keeps logging a value
from becoming a way to run code under someone else's identity once packages
are isolated (Phase 9).

**`Forge` namespace** (frozen; no `process` object): `version`, `platform`,
`args`, `exitCode`, `exit()`, `env.get/has/toObject`, `stdout`/`stderr`
`.write()` and `.isTerminal()`, `addSignalListener` / `removeSignalListener`.
Exit codes are limited to 0-255. `Forge.exit()` unwinds the script with an
uncatchable error that is not reported; pending timers, microtasks and
`finally` blocks do not run. Environment access is not yet permission-checked;
Kavach adds per-variable checks in 8.2.

**Signals and graceful shutdown.** Ctrl+C and Ctrl+Break arrive on a system
thread that only records them, wakes the event loop and, with no listener,
requests an engine interrupt. With a listener, it runs on the main thread when
the loop is idle; without one, the script stops (even inside an endless loop)
and the process exits with 130 (SIGINT) or 149 (SIGBREAK). A second signal
before the first is handled ends the process at once. Closing the console
window keeps the Windows default.

**Other changes.**
- Uncaught errors print as `file:line:col`, then the formatted value and its
  stack, through the console-aware writer.
- The old `Result = <number>` line after a script whose last statement was a
  number is removed; it corrupted program output.
- `moz.build` adds `"."` to `LOCAL_INCLUDES` (mozbuild requires the list
  sorted: `"!.."`, `"."`, `".."`) and the new source files.

**Tests.** Six new self-test groups (34 formatting cases, including getters
and Proxies that would throw if touched), six new runtime tests
(console output, `Forge.args` with a non-ASCII argument, three exit-code
cases, a thrown non-Error), `forge-core/platform/HostTest.cpp` in the MSVC
CI job, and manual Ctrl+C and Unicode-display checks in `tests/manual/`.

**Verified** on Varad's machine: the first real build had one error (the
unsorted `LOCAL_INCLUDES`); after the fix, `build.ps1 -Test` passed all 54
self-tests and 15 runtime tests. HostTest runs only in CI.

## Phase 8.2: Kavach core, the permission checker (2026-11-11)

Forge now denies file and environment access unless it is granted on the
command line (`--allow-read=`, `--allow-write=`, `--allow-env[=A,B]`,
`--audit-log=`). Full user documentation and the permission UX rules (G38)
are in `Kavach.md`.

**What was built**
- `kavach/Ops.h` (R2): one table row per JavaScript-reachable native (49
  rows), with its permission. Natives that check name their row, so a
  missing row is a compile error, and `--self-test` walks everything Forge
  adds to the global and fails on a function without a row or a row
  without a function.
- `kavach/PathRules.cpp` (R3): the one canonicalizer and the grant matcher.
  It is pure string code with Windows semantics, so it also runs in the
  Linux sanitizer job and under libFuzzer. It refuses device and namespace
  paths, drive-relative paths, trailing dots and spaces, `:` (alternate data
  streams), reserved device names (with or without an extension, including
  superscript digits), `< > " | ? *`, `..` above the root, and missing 8.3
  short names.
- `kavach/Kavach.cpp`: the checker, policy, resolver and audit log. A path
  must be covered by a grant as written (nothing is opened and no network
  share is contacted otherwise), then is resolved with
  `GetFinalPathNameByHandleW`, and the real location must be covered too,
  so a junction out of a granted folder is refused. Protected paths (R13):
  `.git`, `forge.json`, `forge.lock`, the audit log and the install folder.
  Grants of a drive root, a share root or the home folder stop startup with
  exit code 2. Environment variables are checked one name at a time.
- Proof tokens: filesystem natives get paths only from `CheckPathArg`,
  which returns a `kavach::CheckedPath` that only the checker can make.
  `ToForgePath` (which called `ToString` on any value) is gone; only strings
  are paths.
- Audit log: JSON lines for every allow and deny, written before the
  operation runs; a rate limit of 1,000 records and then 100 per second per
  op, with the rest counted in `suppressed` records (R15); a failed write
  refuses the operation (R7).
- Refusals are catchable Errors with `code`, `permission`, `op` and
  `path`/`variable`, and a message that names the exact option that would
  allow the operation.

**Also in this step**
- `clearInterval` (missing; required by the WinterTC minimum common API).
- Windows process mitigation policies (R22): no DLLs from network shares or
  low-integrity files, System32 preferred, extension points disabled.
  Self-tested by reading them back.
- Bug fix: `File::CreateDirectories` used to try to create every ancestor up
  to the drive root and ignore "already exists". With the absolute paths
  Kavach now passes, `CreateDirectoryW("C:/")` returns access denied, so
  `mkdirSync(..., { recursive: true })` failed. It now stops at the first
  existing ancestor.
- Benchmarks pass `--allow-read`/`--allow-write` for their two scratch
  folders. Filesystem results from now on include Kavach's checks.

**Tests**
- Self-tests: the op-table walk, env denial, fs denial outside the grant,
  protected paths, 12 ambiguous paths refused, non-string paths refused
  (an object's `toString` must not run), `clearInterval`, mitigations.
- Runtime tests: reads inside and outside the grant, a junction escape
  (created with `mklink /J` in Temp, no admin rights), an uncaught refusal,
  the audit log contents, startup refusals for `C:\` and the home folder,
  and env grants.
- `kavach/PathRulesTest.cpp` in the Linux (ASan, UBSan, C++17 and C++20)
  and MSVC jobs.
- `kavach/fuzz/PathRulesFuzz.cpp`: libFuzzer with invariants (idempotence,
  no `.`/`..`/`/` in output, parent covers child, prefix siblings never
  covered). 1.86 million runs in the sandbox without a failure; a new CI
  job runs it for 60 s on every push.

**Known limits** (see `Kavach.md`): paths are checked, then opened by name
(a race remains until handle-based checks in 8.3); hard links are not yet
detected; one policy for the whole program until Phase 9; the async fs
functions throw refusals instead of rejecting.

**Verified** on Varad's machine: the first run found the CreateDirectories
bug and three escaping mistakes in the new self-test code itself; after the
fixes, `build.ps1 -Test` passed all 61 self-tests and 22 runtime tests.

## Phase 8.3: handle-based filesystem and the full fs API (2026-11-11)

**Handle-based checks (rule R3, "checked on the opened handle; hard links
handled").** `kavach/SecureFs.cpp` performs every file operation on handles,
so nothing changed on disk between Kavach's check and the operation can widen
what it reaches:
- The parent folder is opened and its final path must be exactly the folder
  that was checked; the file is then opened relative to that handle
  (`NtCreateFile` with a root directory). A folder swapped for a junction
  after the check is detected and refused (`changed-after-check`).
- Reads may follow links; the opened handle's final path is re-checked.
- Writes, creation, rename and delete never follow the last link: writing
  through a link is refused, deleting a link deletes the link, and recursive
  delete never descends into one.
- Writing a file with more than one hard link is refused.
- Rename uses `NtSetInformationFile(FileRenameInformationEx)` with the
  destination folder's handle as the root.

**Checker change.** For `rm`, `rename` and `lstat`, which act on a link
itself, Kavach now resolves only the folders above the last component. It
used to follow the last link too, so removing a junction inside a granted
folder was refused because its target was outside.

**New and changed fs API** (`runtime/Fs.h`): `readdir` (with
`{ withFileTypes }`), `rename`, `copyFile`, `lstat`, `realpath`, each sync
and Promise-returning; `rm` removes folders with `{ recursive: true }`;
`stat` returns `type`, `isFile`, `isDirectory`, `isSymbolicLink`, `nlink`
and four timestamps besides `size`; `"utf-8"` is accepted besides `"utf8"`.
A refusal from a Promise-returning function is now a rejected Promise.

**Refactoring.** The fs natives (about 1,900 lines) moved from `forge.cpp`
to `runtime/Fs.cpp`, with one job type for both the sync and async paths.
Shared binding helpers moved to `runtime/JsUtil.cpp`. The op table has 61
rows. Kavach's audit writes are serialized with a mutex, so worker threads
can record late refusals.

**Tests.** A self-test for the new functions; runtime tests for the async
functions (including a rejected refusal) and for links (a real hard link
and junction made with `mklink`: writing the hard link is refused, removing
the junction leaves the outside file intact).

**Verified** on Varad's machine. The first run found a rename bug (the new
name was placed 4 bytes too far into the rename structure, error 123). The
second found the checker following the last link for `rm`. The third passed
all 62 self-tests and 24 runtime tests; the runner's final cleanup then
stopped on an error message from `cmd /c rmdir` for the junction the test
had already removed, which is fixed.

## README: Kavach, standards status, honest benchmark note (2026-11-12)

- `README.md` now leads with the security goal, shows the permission flags in
  the first example (instead of `print`), summarizes Kavach, and states that
  Forge **targets** the WinterTC Minimum Common Web API but does not conform
  yet (rule R11: claims never exceed proof). It notes that the benchmark
  numbers predate Kavach's checks.
- New `WinterTC.md`: the entry-by-entry status against the specification
  (6 of 25 global entries done, 1 partial, 4 provided by the engine but not
  yet tested; the 53 interfaces are WebAssembly-only so far), the phase that
  adds each missing entry, and the planned documented deviations.
