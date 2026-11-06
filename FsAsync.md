# FsAsync — Promise-Returning Filesystem JS API (Phase 7.5)

## Purpose

Specifies the Promise-returning counterparts to `Fs.md`'s `fs.*Sync`
surface — `fs.readFile`, `fs.writeFile`, `fs.appendFile`, `fs.exists`,
`fs.mkdir`, `fs.rm`, `fs.stat` — dispatched onto `ThreadPool` (Phase 4,
real-build-confirmed) so file I/O no longer blocks the single JS
event-loop thread. `JsBindings.md` reserved these exact bare names for
this purpose from the start ("a `*Sync` suffix... exists specifically to
reserve the unsuffixed names for that"), and `Fs.md`'s own Extensibility
section named this phase explicitly. Neither of those two frozen docs
changes here — this is a new, additive spec that depends on both without
modifying either.

This is a materially bigger design problem than Phase 7.1-7.4 were, for
one reason that deserves stating up front: every prior JS-visible
binding in this codebase runs entirely on the single thread that owns
`JSContext cx`. This phase is the first to genuinely involve a second
thread (a `ThreadPool` worker) in the lifecycle of a JS-visible
operation. Every design decision below follows from one hard constraint:
**SpiderMonkey's `JSContext`/GC/`JS::Value` machinery is not thread-safe
and must only ever be touched from the thread that owns `cx`.** A worker
thread doing file I/O must never construct, read, or hold a `JS::Value`,
a `JSObject*`, or anything JSAPI-typed. Every design choice here is
either a direct consequence of that constraint or exists to make it
impossible to violate by accident.

## Responsibilities

- Provide async, Promise-returning equivalents of all seven `fs.*Sync`
  methods, with the same semantics (per Fs.md) minus blocking.
- Perform the actual `File`/`Path` I/O on a `ThreadPool` worker thread,
  never on the main JS thread.
- Get the result (or error) back to the main thread and resolve/reject
  the JS `Promise` there — the only place JSAPI calls are legal.
- Keep the returned `Promise` alive for as long as the operation is
  in flight, independent of whether script itself retains a reference to
  it (a "fire and forget" `fs.writeFile(path, data)` with no `.then()`
  must not have its `Promise` collected mid-flight).
- Make sure the runtime's own event loop does not exit while an async fs
  operation is still in flight with nothing else pending (see Design
  Goals — this is a real, previously-latent gap this phase closes).

## Non-Goals

- **Cancellation.** No `AbortSignal`/`AbortController`-style mechanism.
  A dispatched operation always runs to completion; there is no way to
  cancel a submitted `ThreadPool` task once it starts. Future work if a
  real caller needs it.
- **Streaming.** Same as `Fs.md`: whole-file operations only.
- **Any change to `fs.*Sync` (Phase 7.3, frozen) or `JsBindings.md`
  (Phase 7.2, frozen).** Both are depended on, not modified.
- **A separate `fs.promises` sub-namespace**, unlike Node's
  callback-plus-`fs.promises` split. `JsBindings.md` already settled
  this: the bare name *is* the Promise-returning variant here; there is
  no callback-based flavor at all in this codebase.
- **Any new `ThreadPool` capability** — no priority levels, no
  backpressure/admission control, no dynamic resizing. `ThreadPool` is
  used exactly as Phase 4 built it: `Submit(callable)` onto the existing
  fixed-size pool.
- **Ordering guarantees across concurrent async fs calls.** Two
  concurrently-issued `fs.readFile` calls resolve in whatever order their
  worker tasks and completions happen to finish in — no queue-order or
  submission-order guarantee, matching `ThreadPool`'s own unordered
  worker-pull model.
- **Async variants of anything outside `Fs.md`'s existing seven
  methods.** No new filesystem capability is added; only the blocking
  behavior of the existing seven changes.
- **Changing `IocpLoop`'s public API** (frozen, real-build-confirmed as
  of Phase 6). See Design Goals for where the new bookkeeping this phase
  needs actually lives instead.

## Design Goals

### The thread boundary is absolute

A worker thread's task body may only touch: the resolved `Path`, an
owned `forge::core::Vector<u8>` (for write/append data, or a read
result), `forge::core::File`, and `Result<T>`/`Error`. Nothing else.
Specifically, it must never touch `JSContext`, any `JS::Value`/
`JS::Handle`/`Rooted`, or anything under `js/*`. The main thread does
*all* JS-facing work: parsing arguments into forge-core types before
dispatch, and marshalling the worker's result into a JS value (or a
thrown/rejected error) after the completion is delivered back.

### Write/append data must be copied, not aliased, before dispatch

`Fs.md`'s existing `ResolveWriteData`/`AsByteSpan` (Phase 7.3) return a
`Span<const u8>` that, for the `Uint8Array`/`ArrayBuffer` input case,
*aliases the caller's JS buffer* — `JsBindings.md`'s own frozen contract
for `AsByteSpan` says this is "valid only for the duration of the call".
An async write cannot honor that contract if it hands the same span to a
worker thread that will read it at some arbitrary later time — the JS
buffer could be resized, detached, or collected long before the worker
gets to it. The async path must therefore **copy** the resolved bytes
into an owned `Vector<u8>` synchronously, on the main thread, during the
initial call — before `Submit()`-ing anything — and hand that owned copy
across the thread boundary instead. This is strictly more copying than
the sync path does (which can safely alias for the span's short,
synchronous lifetime), and is the correct, necessary cost of going async
here, not an oversight to optimize away.

### The `Promise` must be GC-rooted independent of script

Once `fs.readFile(...)` returns, script may or may not keep any
reference to the returned `Promise` (a bare `fs.writeFile(path, data);`
statement with no `.then()` is valid and must still complete correctly).
The pending operation's own bookkeeping struct must hold the `Promise`
via a traced GC root (a `JS::Heap<JSObject*>`, matching `Microtask`'s and
`JsTimer`'s existing fields traced by `TraceForgeRoots` since Phase 6) —
not rely on script-side references, which may not exist. This means
every in-flight async fs operation must live somewhere enumerable, so
`TraceForgeRoots` can walk all of them on every GC, the same way it
already walks the microtask queue and the timer registry.

### Cross-thread wakeup reuses `IocpLoop::PostCompletion` — but this exact pattern has never been tested

`IocpLoop::PostCompletion` is explicitly documented ("safe to call...
to wake Run() from another thread") as the mechanism for exactly this
situation, and it already exists, real-build-confirmed, from Phase 6 —
reusing it here, rather than inventing a second cross-thread wakeup
mechanism, is the obvious choice. **However**, re-reading
`IocpSmokeTest.cpp` (the only real-machine confirmation `PostCompletion`
has ever had) shows it posts a completion and calls `RunOnce()`
*immediately afterward, on the same thread* — it has never actually been
exercised with one thread blocked inside `RunOnce()`'s
`GetQueuedCompletionStatus` wait while a *different* thread calls
`PostCompletion`. IOCP is designed and documented by Microsoft for
exactly this cross-thread pattern, so there's good reason to expect it
works — but "well-documented Win32 behavior" has not been this project's
bar for "confirmed" anywhere else (`File`, `Mutex`/`ConditionVariable`,
`Socket` all needed their own real-machine smoke test despite being
equally well-documented APIs), and this phase shouldn't quietly lower
that bar. **This is this milestone's single biggest open technical
risk, and needs its own dedicated smoke-test case** — a real `ThreadPool`
worker calling `PostCompletion` while the main thread is genuinely
blocked in `RunOnce()`, checked on the real machine before this phase can
be called done. See Acceptance Criteria.

### The event loop's exit condition has a real, pre-existing gap this phase must close

`Runtime::run()` (`forge.cpp`) currently exits its loop when
`jobQueue_.empty() && loop_.Empty()`. `IocpLoop::Empty()`'s own doc
comment already flags exactly this gap: it "does not yet account for
pending async I/O operations posted via `AssociateHandle`" and predicts
"a future async variant... will need to add an active-operation count
for that, the same way libuv/Node track 'active handles'." This phase is
that predicted future variant. Without a fix, a script whose *only*
pending work is an in-flight `fs.readFile(...).then(...)` — no timers,
nothing else queued — would see `Runtime::run()` exit (and the process
end) before the read ever completes, silently dropping the operation.
This is a correctness bug, not a performance concern, and it is real
today in the sense that any future async I/O phase (this one, or a later
async `net.*`) will hit it identically if not addressed.

**Fix, scoped narrowly:** rather than changing `IocpLoop` itself (frozen,
real-build-confirmed, and `Empty()`'s own comment defers this
responsibility to the caller rather than mandating it live inside
`IocpLoop`), add a small pending-operation counter to `Runtime` itself
(`forge.cpp`, already the runtime-integration layer modified across
Phases 6-7, not a frozen `forge-core` component) — incremented when an
async fs operation is dispatched, decremented when its completion is
delivered — and extend `run()`'s exit condition to
`jobQueue_.empty() && loop_.Empty() && pendingAsyncOps_ == 0`. This is
the minimal, targeted change; it does not touch `IocpLoop.h`/`.cpp` at
all.

### Native `Promise` creation/resolution is new binding surface

Nothing in this codebase has ever called `JS::NewPromiseObject`/
`JS::ResolvePromise`/`JS::RejectPromise` from C++ before — every existing
use of `Promise` (e.g. `promise-chain-bench.js`) is pure script calling
the engine's own built-in `Promise`. This phase is the first native
construction and settlement of a `Promise` object. This isn't a
Non-Goal or a risk exactly, just a fact worth being explicit about: it
should be verified the same careful way every other new JSAPI surface
in this project has been (real headers read directly, not guessed at,
per the `JS_NewPlainObject`/`Object.h` precedent from Phase 7.2).

### `Result<T>`/no exceptions still apply on the worker side

The worker task's body is exactly as fallible as `fs.*Sync`'s existing
binding logic and returns the same `Result<T>`/`Error` shape — no new
error-handling convention. Only the *delivery* mechanism differs (stored
in the pending-operation struct and marshalled into a `Promise`
rejection on the main thread, instead of thrown directly).

## Public API

All under `globalThis.fs`, alongside the existing `*Sync` methods.
`path` arguments accept a JS string, converted via `ToForgePath`, exactly
as `Fs.md` already specifies for the sync versions — this section only
adds the `Promise` wrapper and async dispatch, not new argument shapes.

```
fs.readFile(path: string, encoding?: "utf8"): Promise<Uint8Array | string>
  // Same encoding semantics as readFileSync. Rejects with the same
  // Error shape (.code/.syscall/.path/.nativeCode) readFileSync throws.

fs.writeFile(path: string, data: string | Uint8Array | ArrayBuffer): Promise<void>
  // data is copied to an owned buffer synchronously, on the calling
  // thread, before any dispatch — see Design Goals. The write itself
  // (including the short-write retry loop WriteAllBytes already
  // implements) runs on a ThreadPool worker.

fs.appendFile(path: string, data: string | Uint8Array | ArrayBuffer): Promise<void>
  // Same as writeFile but File::Open(path, FileMode::Append).

fs.exists(path: string): Promise<boolean>
  // Same File::Exists semantics and Node-divergence rationale as
  // existsSync (Fs.md): a genuine File::Exists failure rejects rather
  // than resolving false.

fs.mkdir(path: string, options?: { recursive?: boolean }): Promise<void>
  // Same File::MakeDirectory / File::CreateDirectories split as mkdirSync.

fs.rm(path: string): Promise<void>
  // Same File::Remove (file-only) semantics as rmSync.

fs.stat(path: string): Promise<{ size: number }>
  // Same File::Open + SizeInBytes composition as statSync.
```

## Ownership

Each in-flight async operation owns: a heap-allocated bookkeeping struct
(embedding an `IoCompletion` as its first member, per that type's own
documented zero-vtable-call idiom, so the completion callback can
`reinterpret_cast` straight back to it with no separate lookup table
needed — this phase has no script-visible cancellation, so unlike
`JsTimerRegistry`'s `HashMap<int, ...>` keyed for `clearTimeout`, no
ID-based lookup is required here); a `JS::Heap<JSObject*>` holding the
`Promise` (GC-rooted per Design Goals); whatever owned, forge-core-only
inputs the worker needs (a `Path`, an owned `Vector<u8>` for write/append
data); and a place to store the worker's `Result<T>` once it completes.

This struct is allocated (via `memory::MakeUnique`, `Result<UniquePtr<T>>`
on OOM, matching Phase 6's established allocation convention) on the main
thread before dispatch, and freed on the main thread inside the
completion callback, after the `Promise` has been resolved or rejected —
never freed from the worker thread. Every in-flight instance must be
reachable from *some* enumerable collection Owned by `Runtime` (exact
container is an implementation-stage choice — a `Vector`/`HashMap` of raw
or owning pointers both work; unlike `JsTimerRegistry` there's no
per-item ID lookup requirement, so this doesn't need to match that
structure) so that (a) `TraceForgeRoots` can trace every pending
`Promise` root, and (b) `Runtime` can report a pending-operation count
for `run()`'s exit condition (see Design Goals).

## Error Handling Policy

Every failure surfaces as a `Promise` rejection with the exact same
shape `ThrowJsError` already produces for the sync methods
(`.code`/`.syscall`/`.path`, plus `.nativeCode` where applicable) — this
phase changes *delivery* (rejection instead of a synchronous throw), not
the error shape itself. The `Error` JS object must still be constructed
on the main thread (it needs `cx`), from the `Error`/`ErrorCode` the
worker thread produced and stored in the pending-operation struct.

## Thread Safety

This is the one component in the whole `fs` surface where thread safety
is the central design question, not an inherited default:

- Exactly one thread (the `ThreadPool` worker assigned the task) touches
  the operation's `File`/`Path`/`Vector<u8>` inputs and produces its
  `Result<T>`.
- Exactly one thread (the main JS thread, inside the `IoCompletion`
  callback `IocpLoop` dispatches from `RunOnce()`) touches `cx`,
  constructs/marshals JS values, and resolves/rejects the `Promise`.
- The pending-operation struct is written by the worker thread (storing
  its `Result<T>`) and then handed off via `PostCompletion` — there is no
  concurrent read/write of the same field from both threads at once; the
  handoff *is* the synchronization point (matching how `IoCompletion`'s
  existing real-I/O completions already work, just with `ThreadPool`
  standing in for a real device driver).
- Multiple async fs operations may be in flight on different worker
  threads simultaneously; each has its own independent bookkeeping
  struct. No shared mutable state between concurrent operations beyond
  `ThreadPool`'s own already-verified internal queue/mutex.

## Dependencies

Allowed dependencies:

- `ThreadPool` (Phase 4, real-build-confirmed) — `Submit()`.
- `File`/`Path` (Phase 3, real-build-confirmed).
- `JsBindings.md`'s six helpers (Phase 7.2) — `ThrowJsError`,
  `ToForgePath`, `Uint8ArrayFromBytes`, `AsByteSpan`, etc. — reused for
  main-thread argument parsing and result marshalling exactly as `Fs.md`
  already does.
- `Fs.md`'s existing binding logic (Phase 7.3) — `ResolveWriteData`'s
  parsing logic is reused, adapted to produce an owned copy (see Design
  Goals) rather than (only) a borrowed span.
- `IocpLoop::PostCompletion`/`IoCompletion` (Phase 6, real-build-confirmed
  for the same-thread case; cross-thread case unverified until this
  phase's own smoke test — see Design Goals).
- `JS::NewPromiseObject`/`JS::ResolvePromise`/`JS::RejectPromise`
  (`js/Promise.h`) — new to this codebase; read the real header directly
  before implementing, per this project's established practice.
- `Runtime`'s event loop (`forge.cpp`) — needs the pending-operation-count
  fix described in Design Goals.

Forbidden:

- Any JSAPI call from a `ThreadPool` worker thread.
- Aliasing a JS-owned buffer across the thread boundary (see Design
  Goals — always copy for write/append).
- Any change to `IocpLoop.h`/`.cpp`'s public API (the fix lives in
  `Runtime`, not there).

## Extensibility

- A future async `net.*` API will need the identical
  worker-thread/main-thread split, the identical GC-rooting-of-pending-
  promises pattern, and the identical `Runtime`-level pending-operation
  counter this phase introduces — this phase's design (and its
  eventual implementation) should be written with an eye toward that
  reuse, even though `net.*` itself is out of scope here.
- Cancellation (`AbortSignal`), if ever needed, would most naturally
  attach to the pending-operation struct this phase introduces.

## Acceptance Criteria

- All seven methods implemented, matching `Fs.md`'s existing semantics
  exactly, each returning a genuine `Promise` (not a callback, not a
  microtask-only "async-looking but actually synchronous" shim).
- Every method's actual file I/O demonstrably runs on a `ThreadPool`
  worker thread, not the main thread — verified by a real-machine test
  that would visibly block (or fail a "did the main thread stay
  responsive" check) if it were secretly still synchronous.
- **A dedicated smoke test for the cross-thread `PostCompletion` pattern
  specifically** (see Design Goals) — a real `ThreadPool` worker posting
  a completion while the main thread is genuinely blocked inside
  `RunOnce()`'s wait, confirmed on the real machine. This is new
  territory `IocpSmokeTest.cpp` never covered and must not be assumed
  correct by analogy alone.
- A real self-test exercising every method's success and failure path
  through an actual resolved/rejected `Promise` — note this is a
  structurally new requirement for `forge --self-test`, whose existing
  cases (Phase 7.2/7.3) all call into binding functions directly and
  check the result synchronously; these tests must instead let
  `Runtime::run()` actually drive the event loop to completion so the
  `ThreadPool` completion and `Promise` resolution genuinely happen, not
  just call the binding function and inspect a `Result<T>` in place.
  Flagged here explicitly as an implementation-stage question to solve,
  not assumed solvable by reusing the existing harness unchanged.
- A test confirming the `Runtime::run()` exit-condition fix: a script
  whose only pending work is a single in-flight async fs call (no
  timers) must run to completion and exit cleanly, not exit early.
- Benchmarked via a new `bench/fs-async-bench.js`, same
  `run-benchmarks.ps1` methodology as every other script, comparing
  against Node/Bun's `fs.promises` equivalents.
- All of the above through a real `python mach build` — same standard as
  every other phase.

## Implementation Status

Frozen (reviewed and accepted as-is, no unresolved design questions) and
now implemented and real-build-confirmed end to end, milestone by
milestone — see `ROADMAP.md`'s Phase 7.5 entry for the full breakdown and
`HISTORY.md` for each individual confirmation's details:

- **7.5.1 — done, real-machine-confirmed.** The cross-thread
  `PostCompletion` wakeup pattern this whole phase depends on
  (`IocpCrossThreadSmokeTest.cpp`): a genuine `ThreadPool` worker thread
  posting a completion while the main thread is blocked inside
  `RunOnce()`'s wait actually wakes it.
- **7.5.2 — done, real-build-confirmed.** `Runtime::run()`'s
  event-loop-lifetime fix (the `pendingAsyncOps_` counter) — confirmed
  against a genuine in-flight async operation once 7.5.3 gave it a real
  caller, not just proven inert.
- **7.5.3 — done, real-build-confirmed.** `fs.readFile`, the first native
  `JS::Promise` use in this codebase (`AsyncFsReadOp`), both encodings'
  success paths and the `NotFound` rejection path all confirmed.
- **7.5.4 — done, real-build-confirmed.** The remaining six methods
  (`writeFile`/`appendFile`/`mkdir`/`rm`/`exists`/`stat`), sharing one
  `AsyncFsMiscOp` struct discriminated by a `kind` enum rather than
  repeating the pattern six more times. All seven methods specified
  above now exist and work.
- **7.5.5 — done, real-build-confirmed.** A genuine Promise-based
  self-test harness (`RunFsAsyncSmokeTests` in `forge.cpp`, run from
  `forge --self-test`) — the first self-test suite in this codebase that
  actually calls `Runtime::run()` rather than inspecting a `Result<T>`
  synchronously. Includes the dedicated, isolated regression test for
  the 7.5.2 exit-condition fix this Acceptance Criteria section calls
  for above (a script whose only pending work is one in-flight
  `fs.readFile`, no timers). Supersedes `manual-test-fs-async.js` (the
  interim, non-`--self-test` script used to verify 7.5.3/7.5.4 before
  this harness existed) as this feature's ongoing regression check.
- **7.5.6 — in progress.** `bench/fs-async-bench.js` drafted (mirrors
  `fs-bench.js`'s iteration counts/payload for a direct comparison,
  chained through `.then()` rather than a blocking loop — see that
  script's own header comment for why concurrent calls over the shared
  round-trip path would be a genuine race, not a style choice); not yet
  run on the real machine. `Fs.md`/`JsBindings.md`/`PROJECT_CONTEXT.md`
  updated to point at this doc's now-real-build-confirmed status;
  `Forge_Benchmark_Report.md`'s own results table and analysis are
  pending the real benchmark run.

Every method's actual file I/O was confirmed running on a `ThreadPool`
worker thread via the pattern documented in 7.5.1's smoke test and
exercised for real by every subsequent milestone's own real-build run —
no part of this phase was ever silently found to be secretly synchronous.
