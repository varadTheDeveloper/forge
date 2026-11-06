# Forge Core Roadmap

Order of remaining work. Do not start a later phase's component ahead of an
earlier one unless a specific runtime need pulls it forward — note the
reason in `HISTORY.md` if that happens.

Completed components (Types, Error, Failure, Result/ResultVoid, Assert,
Allocator/DefaultAllocator, UniquePtr/MakeUnique, Vector) are out of scope
here — see `PROJECT_CONTEXT.md` → Current Progress and `HISTORY.md` for
their status.

---

## Phase 1 — Core containers, continued — DONE (2026-07-27)

- String — done
- StringView — done
- Span — done

String follows the same rule as everything else: constructors never
allocate; allocation-requiring construction goes through `String::Create(...)`.
UTF-8 internally per the project philosophy.

Implemented in `forge/forge-core/Span.h`, `StringView.h`, `String.h`/
`String.inl`, mirroring `Vector<T>`'s own growth/reserve/copy-truncates-
on-OOM conventions exactly, but always null-terminated for C interop.
Verified with `StringTest.cpp` (13 scenarios, including allocator-failure
paths via a `FailingAllocator`) under the real project constraints —
`g++`/`clang++ -std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic
-Werror` (plus `-Wc++20-extensions` on clang) — and clean under
ASan+UBSan and `valgrind --leak-check=full`. See `HISTORY.md` for the two
real bugs this verification pass caught (a missing `Result.h` include,
and a hidden-friend/ADL gap in `String`'s comparison operators) before
this could be called done. Not yet retested via a real `mach build` —
these are pure header/`.inl` files with no SpiderMonkey dependency, so
the sandbox verification here is materially more trustworthy than the
`forge.cpp`/event-loop work was, but per `AGENTS.md` this still isn't
"confirmed" until it's gone through the actual build.

## Phase 2 — Collections — DONE (2026-07-29)

- Array — done
- HashMap — done
- HashSet — done
- Queue — done
- Stack — done

Implemented in `forge/forge-core/Array.h`, `Hash.h`, `Stack.h`/`.inl`,
`Queue.h`/`.inl`, `HashMap.h`/`.inl`, `HashSet.h`/`.inl`. Design notes:

- `Array<T, N>` is a plain aggregate (public `T data_[N]`) rather than
  following every other container's private-members-plus-accessors
  convention — unlike Vector/String/Queue/HashMap, a fixed-size array has
  no size/capacity invariant to protect, so there's nothing an accessor
  would be guarding. `N == 0` is a separate specialization (a zero-length
  C array isn't valid C++).
- `Stack<T>` is a thin LIFO adapter directly over `Vector<T>` — no new
  storage logic, just a restricted API (push/pop/top only) that documents
  intent at the call site.
- `Queue<T>` is a real circular buffer (not Vector-backed) — a
  Vector-backed FIFO would need an O(n) shift on every pop. Same
  Allocator/Reserve/copy-truncates-on-OOM conventions as Vector<T>, plus
  a `head_` index and modulo-based wraparound.
- `Hash<T>` (new, `Hash.h`) is the trait `HashMap`/`HashSet` hash keys
  through. The primary template is deliberately left undefined — a key
  type without an explicit specialization fails to compile rather than
  silently hashing raw bytes (including padding) or failing with a
  confusing error deep inside the table. Integer specializations run
  through a splitmix64-style finalizer before masking to a table index,
  so sequential keys (0, 1, 2, ...) don't cluster under the power-of-two
  table sizes below.
- `HashMap<K, V>` is open addressing with linear probing and tombstones
  for deletion — the classic, well-understood design over something
  cleverer (Robin Hood / SwissTable-style), per PROJECT_CONTEXT.md's
  "readability over cleverness". Table size is always a power of two
  (masking, not modulo, for the index); grows (doubles) once
  `size + tombstones` crosses 75% of capacity, counting tombstones
  because they cost probe length exactly like real entries do. No custom
  hasher/key-equality template parameter and no const iteration yet —
  both are easy to add later behind a real use case; left out to keep
  the first version's surface small.
- `HashSet<K>` is a thin composition wrapper around
  `HashMap<K, detail::Unit>`, not a second independent table
  implementation — any future fix to HashMap's probing/growth logic
  applies to HashSet for free.

Verified with five new test files (`ArrayTest.cpp`, `StackTest.cpp`,
`QueueTest.cpp`, `HashMapTest.cpp`, `HashSetTest.cpp` — 34 scenarios
total, including a 1000-entry HashMap growth/erase/rehash stress test and
allocator-failure paths via `FailingAllocator`) under the same real
project constraints as Phase 1: `g++`/`clang++ -std=c++17 -fno-exceptions
-Wall -Wextra -Wpedantic -Werror` (plus `-Wc++20-extensions` on clang),
clean under ASan+UBSan and `valgrind --leak-check=full` (0 leaks, 0
errors across all five), plus an `-O2` pass. Same caveat as Phase 1: not
yet retested via a real `mach build` — pure header/`.inl` files, no
SpiderMonkey dependency, so this sandbox verification is materially more
trustworthy than the `forge.cpp`/event-loop work's was, but per
`AGENTS.md` that's still not "confirmed" until it's gone through the
actual build.

## Phase 3 — Filesystem — DONE and real-build-confirmed (2026-07-29)

- `Path.md` (spec) — done
- `File.md` (spec) — done
- `Path` (`Path.h`/`.inl`) — done, fully verified
- `File` (`File.h`/`.cpp`) — done, **real-build-confirmed**. The
  `CreateDirectory`→`MakeDirectory` rename (see below and `HISTORY.md`)
  compiled clean on the user's real `mach build`; Phase 3 is complete.

The earlier `forge/platform/` groundwork was deliberately removed (see
`HISTORY.md`) — this phase started from a clean slate. Two frozen spec
docs were written first, per this phase's own requirement:
`Path.md` (pure, portable path manipulation — no OS calls) and
`File.md` (actual filesystem I/O — Win32-only, synchronous). The split
mirrors `StringView`/`String`: one type never touches the OS, the other
is entirely the OS.

`Path` is pure `String`/`StringView` manipulation (Join, Parent,
FileName, Stem, Extension, Normalize's `.`/`..`/repeated-separator
handling, IsAbsolute for Windows drive-letter/UNC roots) with zero OS
dependency, so it got the exact same verification treatment as every
other Phase 1/2 container: `g++`/`clang++ -std=c++17 -fno-exceptions
-Wall -Wextra -Wpedantic -Werror` (+ `-Wc++20-extensions` on clang),
clean under ASan+UBSan and `valgrind --leak-check=full`, plus `-O2`.
Added `StringView::RFind` (last-occurrence search) as a backward-
compatible extension — needed for finding the final path separator,
and StringView was never declared frozen the way `Error` was.

`File` (`Open`/`Read`/`Write`/`Seek`/`Tell`/`SizeInBytes`/`Close`, plus
static `Exists`/`MakeDirectory`/`CreateDirectories`/`Remove`/
`ReadAllBytes`/`ReadAllText`) is Win32-only and synchronous by design —
see `File.md`'s Non-Goals for why async is deliberately out of scope for
now. **This component could not be compiled or run in the sandbox this
work was done in at all** — there is no Windows SDK, and no MinGW
cross-compiler could be installed (this sandbox's network access is
allowlisted to package registries, not the Ubuntu `universe` component
MinGW ships in). Verification here was: careful manual review against
the real Win32 API shape, plus a hand-written, type-check-only mock
`windows.h` (same technique used for `IocpLoop` last phase) that let a
real compiler at least confirm `File.cpp` compiles AND links cleanly
against realistic Win32 function signatures under the full `-std=c++17
-fno-exceptions -Wall -Wextra -Wpedantic -Werror` bar — this catches
typos/wrong-argument-count/wrong-type mistakes, but proves nothing about
actual runtime behavior (the mock's functions are dumb stubs).

The user then ran a real `mach build`, which is exactly the kind of
verification the mock couldn't provide, and it caught a real bug the
mock missed entirely: the original method name `File::CreateDirectory`
collided with `<windows.h>`'s `fileapi.h` macro
(`#define CreateDirectory CreateDirectoryA`), a classic Win32
ANSI/Wide dispatch macro that silently rewrites any symbol with that
exact name regardless of namespace/class scope. Fixed by renaming to
`File::MakeDirectory` everywhere (header, implementation, the internal
call site in `CreateDirectories()`, and `File.md`). See `HISTORY.md`
for the full build error and reasoning. The user then re-ran `mach
build` and it completed successfully — the fix, and the `moz.build`
`File.cpp` SOURCES entry, are now real-build-confirmed. Phase 3 is
done. `FileSmokeTest.cpp` (matching `IocpSmokeTest.cpp`'s precedent) is
still available to build and run on the real machine whenever the user
wants the deeper functional coverage (round-tripping content through
`Open`/`Write`/`Read`, `Append`, `Seek`/`Tell`, `CreateNew`/`NotFound`
error paths) beyond what a successful compile alone confirms, but this
is no longer blocking — the compile succeeding was the open question.

## Phase 4 — Threading — DONE, fully real-build-confirmed (2026-07-30)

- `Sync.md` (spec: Mutex, ConditionVariable, LockGuard<T>) — done
- `Thread.md` (spec: Thread, ThreadPool, ErasedCallable) — done
- `ErasedCallable`/`LockGuard<T>` — done, fully verified (pure logic, no
  OS dependency)
- `Mutex`/`ConditionVariable`/`Thread`/`ThreadPool` — implemented,
  **NOT yet confirmed** (see below)

Reviewed the codebase first: no threading/synchronization code existed
anywhere in `forge-core/` before this phase, so there was nothing to
duplicate. Two frozen specs were written before implementation, per
this project's established process: `Sync.md` and `Thread.md`.

`Mutex` wraps a Win32 `SRWLOCK`, `ConditionVariable` wraps a
`CONDITION_VARIABLE`, both stored behind an opaque `void*` in their
headers (both are documented, ABI-stable, one-pointer-sized values
valid when zero-initialized) so `<windows.h>` stays confined to their
`.cpp` files, mirroring `File.h`'s `HANDLE`-as-`void*` precedent.
Neither can fail to construct or lock, so only `Wait`/`WaitFor` return
`Result` — everything else is `void`. `LockGuard<Lockable>` is a
template on the lockable type specifically so its RAII logic could be
verified against a fake test double in this sandbox even though `Mutex`
itself cannot be compiled here — same Path/File-style split as Phase 3.

`Thread` wraps `CreateThread`, move-only like `File`. Every `Thread`
that was ever successfully created must be `Join()`'d or `Detach()`'d
before destruction/move-assignment, enforced with `FORGE_ASSERT`
(matching `std::thread`'s contract) rather than silently
auto-detaching/auto-joining, which would hide a real bug instead of
surfacing it.

`ErasedCallable` (new, `ErasedCallable.h`) is the type-erasure helper
`Thread::Create`/`ThreadPool::Submit` both use to accept an arbitrary
`Callable` and hand it across an OS boundary that only accepts a fixed
`void(*)(void*)` shape — pure C++, zero OS dependency, so like `Path`
and `LockGuard` it got full sandbox verification (`ErasedCallableTest.cpp`:
invoke-runs-and-frees, destroy-without-invoking, a move-only capture, an
OOM path via `FailingAllocator` — clean under both compilers'
`-std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic -Werror`,
ASan+UBSan, `valgrind --leak-check=full`, `-O2`).

`ThreadPool` is a fixed-size worker pool over a `Queue<Task>` guarded by
a `Mutex`+`ConditionVariable`. Deliberately **non-movable**, following
`IocpLoop`'s "default-construct, then `Initialize()`" shape rather than
a `Create()` factory returning the pool by value — every worker's loop
captures a pointer back to the pool, so it needs a stable address from
the moment the first worker spawns, which a value-returning factory
requiring movability cannot safely provide. See `Thread.md`'s Design
Goals for the full reasoning; this was worked out *before* writing code
that would have needed to move a non-movable type through `Result<T>`.

Two real bugs were caught during self-review (see `HISTORY.md` for the
full detail): a friend-access mistake in an early `ConditionVariable.cpp`
draft (private access doesn't extend to free functions, only to the
actual friended class's own members), and a rollback-path deadlock risk
in `ThreadPool::Initialize` (joining an orphaned worker thread before
telling it to stop). Also extracted `platform::TranslateWin32Error` out
of `File.cpp` (which had its own private copy) into a shared
`platform/Win32Error.h`/`.cpp`, since `ConditionVariable.cpp`/`Thread.cpp`
needed the identical mapping and a third independent copy would have
been real duplication.

**This component could not be compiled or run in the sandbox this work
was done in at all** — same constraint as `File`/Phase 3. Verification
here was: careful manual review, plus the hand-written mock
`windows.h` (extended with `SRWLOCK`/`CONDITION_VARIABLE`/
`CreateThread`/`WaitForSingleObject` and friends) confirming every new
`.cpp` compiles AND links cleanly, including a small driver that forces
every template (`Thread::Create<Callable>`, `ThreadPool::Submit<Callable>`,
`MakeErasedCallable<Callable>`) to actually instantiate rather than just
compiling each file in isolation — that driver also ran clean under
ASan+UBSan and `valgrind --leak-check=full` (0 leaks), which is a real
signal about the erasure/cleanup logic even though the mock's
`CreateThread` is a dumb stub. **This must be verified on the real
machine before being called "done"** — `ThreadingSmokeTest.cpp`
(matching `FileSmokeTest.cpp`'s precedent) is ready to build and run
there: a `Mutex` mutual-exclusion stress test (8 threads × 20000
increments each), a `ConditionVariable` producer/consumer handoff and
`WaitFor` timeout check, and a `ThreadPool` test (4 workers, 500 tasks,
verifying every one ran exactly once, plus `Submit` after `Shutdown()`
correctly failing).

The first real `mach build` attempt against the Phase 4 `moz.build`
change didn't even reach the compiler — it failed parsing `moz.build`
itself with `mozbuild.util.UnsortedError`. Mozbuild's `SOURCES` list
requires case-insensitive sorted order; the new mixed-case entries
(`Mutex.cpp`, `Thread.cpp`, `ThreadPool.cpp`, `Win32Error.cpp`) had been
placed in raw-ASCII sorted order instead, which disagrees with
case-insensitive order exactly where an uppercase-initial filename
sits next to a lowercase-initial path (e.g. `Mutex.cpp` vs. `memory/...`).
Fixed by re-sorting the list case-insensitively — see `HISTORY.md` for
the full error and the corrected order. Not yet re-verified: needs
another real `mach build` pass to confirm `moz.build` now parses and
the actual compile proceeds.

**Re-checked 2026-07-30, before resuming this phase:** re-read the live
`moz.build` directly off the device and confirmed programmatically
(`sorted(sources, key=str.lower)`) that its current `SOURCES` order
exactly matches case-insensitive sort — the fix described above is
genuinely in place, not just described. Also found and fixed a real
latent bug, before the user could hit it: `ThreadingSmokeTest.cpp`'s and
`FileSmokeTest.cpp`'s own header-comment `cl` build commands mixed
unprefixed paths (`Mutex.cpp`, `File.cpp`, `platform\Win32Error.cpp` —
correct if run from within `forge-core\`) with a redundant
`forge-core\`-prefixed path for `memory\DefaultAllocator.cpp`/
`memory\detail\AllocationBackend.cpp` in the same command line — running
either command verbatim from `forge-core\` (the natural reading) would
have failed with a file-not-found error on those two entries. Fixed both
comments to drop the redundant prefix, consistent with every other path
in the same command. (`SocketSmokeTest.cpp`'s build comment has the
identical bug — left alone for now since Phase 5 isn't in scope this
round, but flagged here so it isn't a surprise later.)

**Progress, 2026-07-30 — real MSVC compile of the production sources,
for the first time ever:** the user ran the standalone `cl` command from
`forge-core\` (Visual Studio Developer PowerShell) covering
`ThreadingSmokeTest.cpp` plus every production dependency it needs
(`Mutex.cpp`, `ConditionVariable.cpp`, `Thread.cpp`, `ThreadPool.cpp`,
`platform\Win32Error.cpp`, `memory\DefaultAllocator.cpp`,
`memory\detail\AllocationBackend.cpp`). All seven production sources
compiled clean under `/std:c++17 /EHs- /W4` against the *real*
`<windows.h>` — the first time any of them has been compiled by a real
compiler at all (every prior verification pass used a hand-written mock
`<windows.h>`, per the entries above). This alone is real, new evidence:
the Win32 API usage in these five files is not just plausible against a
hand-rolled mock, it is genuinely correct against the actual SDK headers.

Only `ThreadingSmokeTest.cpp` itself failed to compile — a real MSVC-
specific bug, not a production-code bug: `RunMutexStressTest`'s lambda
(`[&mutex, &counter]() { for (int j = 0; j < kIterations; ++j) ... }`)
read the enclosing `constexpr int kIterations` without capturing it.
GCC/Clang exempt a `constexpr` local from needing capture when it's only
read (not odr-used) — which is why this passed clean in every sandbox
compile this session — but MSVC's frontend requires an explicit capture
regardless. Fixed by adding `kIterations` to the capture list
(`[&mutex, &counter, kIterations]`); reviewed every other lambda in the
file for the same pattern and found no other instances. Since the smoke
test didn't compile, linking never happened and nothing has actually run
against real threads yet.

**`ThreadingSmokeTest.cpp` now real-thread-confirmed (2026-07-30).** The
user rebuilt with the capture fix — clean compile, no errors — and ran
the resulting `.exe`. Output:

```
RunMutexStressTest...
RunConditionVariableProducerConsumerTest...
RunConditionVariableTimeoutTest...
RunThreadPoolTest...
ThreadingSmokeTest: all checks passed
```

All four real-thread scenarios passed against actual OS threads, not a
mock: the 8-thread × 20000-increment `Mutex` stress test came out with
the exact expected count (real mutual exclusion, not a lucky race), the
`ConditionVariable` producer/consumer handoff and `WaitFor` timeout both
behaved correctly, and the 4-worker/500-task `ThreadPool` run completed
every task exactly once, including `Submit` after `Shutdown()` correctly
failing. This closes the one thing sandbox verification could never
provide — real OS-scheduler behavior for this code.

**`python mach build` now confirmed (2026-07-30).** The user ran it after
the `ThreadingSmokeTest.cpp` fix above, and it completed successfully —
the one remaining piece this document specifically meant by
"real-build-confirmed," now closed. This is the first time `mach build`
has ever actually compiled `Mutex.cpp`/`ConditionVariable.cpp`/
`Thread.cpp`/`ThreadPool.cpp`/`platform/Win32Error.cpp` through Gecko's
own toolchain integration, and it confirms — not just "believed correct"
— that the case-insensitive `SOURCES` sort fix (see the "`moz.build`
SOURCES ordering bug" entry above) genuinely resolves the earlier parse
failure and lets the build proceed.

**Phase 4 is now fully real-build-confirmed, all three legs together:**

1. `python mach build` — the production Win32 threading code compiles
   clean through the real Gecko/`moz.build` toolchain integration.
2. A standalone MSVC `cl` compile of the same production sources against
   the real `<windows.h>` (not a mock) — confirms the Win32 API usage
   itself, independent of `moz.build`.
3. `ThreadingSmokeTest.cpp`, rebuilt and run — confirms real-thread
   *behavior*: correct mutual exclusion under contention, correct
   condition-variable wait/notify/timeout, and a thread pool that runs
   every submitted task exactly once and correctly rejects work after
   shutdown.

No sandbox-only claim remains for any of `Mutex`/`ConditionVariable`/
`Thread`/`ThreadPool` — every piece of this phase's Win32-backed
implementation has now been checked by the real toolchain, the real
Win32 API, and real OS threads. `ErasedCallable`/`LockGuard<T>` remain
exactly as they were: fully verified from the start (pure C++, no OS
dependency, so ordinary sandbox testing was always sufficient for
those two). Phase 4, in its entirety, is done.

Both steps need to succeed, and both outputs need to be reported back,
before Phase 4 can be marked real-build-confirmed.

## Phase 5 — Networking — DONE, fully real-build-confirmed (2026-07-30)

`Socket`/`IpAddress` (`forge-core/Socket.h/.cpp`, `IpAddress.h/.inl`, specs
in `Socket.md`/`IpAddress.md`) exist on disk with substantial
implementations, but **`moz.build`'s `SOURCES` list did not include
`Socket.cpp`** (confirmed 2026-07-30 by reading the live file directly off
the device — see HISTORY.md's "Phase 6 corrected" entry) and neither this
file nor `PROJECT_CONTEXT.md`'s "Completed" list had ever been updated to
mark it done. Any earlier report that Phase 5 was "real-build-confirmed"
did not reflect what was actually on disk.

**Wiring done this round, before asking for a real build** (same
"re-check what's actually true, then fix what's broken before handing it
to the user" approach used when Phase 4 was resumed):

1. Added `forge-core/Socket.cpp` to `moz.build`'s `SOURCES`, in the
   correct case-insensitive sorted position (`platform/Win32Error.cpp` <
   `Socket.cpp` < `Thread.cpp`, confirmed programmatically the same way
   the Phase 4 sort fix was) — this is the actual missing piece that made
   Phase 5 "not started" from a real-build perspective.
2. Added a new `OS_LIBS += ["ws2_32"]` entry to `moz.build`. `Socket.cpp`
   carries `#pragma comment(lib, "ws2_32.lib")` as a belt-and-suspenders
   convenience, but its own comment says the *authoritative* linker input
   is meant to be a `moz.build` `OS_LIBS` entry "added alongside this
   file" — that entry had never actually been added. Without it, linking
   `forge.exe` after adding `Socket.cpp` to `SOURCES` risked an unresolved-
   external-symbol failure at the link step (`WSAStartup`/`socket`/
   `connect`/etc.), a different and later failure than the missing-
   `SOURCES`-entry problem, and one the pragma alone might or might not
   have papered over depending on how Gecko's build wrapper invokes the
   linker. Added properly rather than relying on the pragma.
3. Reviewed `SocketSmokeTest.cpp` (never previously run, matching
   `ThreadingSmokeTest.cpp`'s situation before this session) and found
   two more real bugs before handing it back, neither ever exercised by
   a real compiler until now:
   - The same class of MSVC-specific lambda-capture bug just fixed in
     `ThreadingSmokeTest.cpp` (`kIterations`, C3493): the server thread's
     lambda in `RunSocketEchoTest` reads the enclosing
     `constexpr Size kPayloadSize` (as an array bound and a `Span` byte
     count) without capturing it. GCC/Clang don't require capturing a
     `constexpr` local that's only read, which is why this was never
     caught in any sandbox pass — MSVC does. Fixed the same way: added
     `kPayloadSize` to the capture list.
   - The file's own example build command listed `IpAddress.inl` as a
     source file to compile directly. That's wrong: like every other
     `.inl` in this codebase (`Path.inl`, `Queue.inl`, etc.), it's meant
     to be `#include`d by its header (`IpAddress.h` does this at its own
     end), not compiled as its own translation unit — its member-function
     bodies reference `IpAddress`/`Endpoint`, which aren't declared until
     `IpAddress.h` has already been processed. Compiling it standalone
     would fail with undeclared-identifier errors. Removed it from the
     command; `Socket.cpp`'s own `#include "IpAddress.h"` already brings
     in everything needed. Also fixed the same redundant `forge-core\`-
     prefix path bug found in `ThreadingSmokeTest.cpp`/`FileSmokeTest.cpp`
     (the command assumes invocation from `forge-core\`, matching every
     other path in the same line).

No production `Socket.cpp`/`Socket.h`/`IpAddress.h`/`IpAddress.inl` code
changed — the implementation itself was already carefully reviewed and
sandbox-verified (see `Socket.md`'s Acceptance Criteria) before this
round; only `moz.build` and `SocketSmokeTest.cpp`'s build comment/lambda
capture changed.

**Real MSVC compile of `Socket.cpp` succeeds (2026-07-30).** The user ran
the `cl` command above; all seven production sources compiled clean,
including `Socket.cpp` against the real `<winsock2.h>`/`<ws2tcpip.h>` for
the first time (every prior pass used a hand-written mock) — the same
category of real, new evidence as Phase 4's equivalent milestone.

`SocketSmokeTest.cpp` itself failed with `error C2131: expression did
not evaluate to a constant` at its `u8 received[kPayloadSize]{}` array
declaration. This was a direct consequence of the previous fix in this
entry being incomplete: adding `kPayloadSize` to the lambda's capture
list satisfied MSVC's odr-use rule, but capturing a variable also
changes what its name refers to *inside* the lambda — every use becomes
the closure's own copied (non-`constexpr`) data member, not the original
constant. That's fine for a loop bound (`ThreadingSmokeTest.cpp`'s
`kIterations` fix, which only needs a value), but breaks an array bound
(needs a genuine constant expression). **Fixed properly this time**: moved
`kPayloadSize` out of the function to file scope (inside the anonymous
namespace), so no lambda needs to capture it at all — a file-scope
constant is never a captured local, so every use, array bound or not, is
unambiguously the real constant. See `HISTORY.md`'s "Real MSVC compile of
Socket.cpp succeeds" entry for the full explanation.

**`SocketSmokeTest.cpp` now real-network-confirmed, and `python mach
build` succeeded (2026-07-30).** The user rebuilt with the file-scope fix
— clean compile — and ran the resulting `.exe`:

```
RunSocketEchoTest...
RunSocketConnectRefusedTest...
SocketSmokeTest: all checks passed
```

Both scenarios passed against a real OS TCP stack: a real loopback
listener/client echo round-trip (`Listen`/`Accept`/`Connect`/`Send`/
`Receive`/`Close` all exercised end to end, byte-for-byte payload
verified), and a real connect-to-nothing-listening failure handled
cleanly as a `Result` error rather than a crash or hang. Separately, the
user ran `python mach build` and it completed successfully — the first
time `Socket.cpp` has compiled and linked through Gecko's own toolchain,
confirming both the `SOURCES` addition and the new `ws2_32` `OS_LIBS`
entry actually work end to end.

**Phase 5 is now fully real-build-confirmed, all three legs together:**

1. `python mach build` — `Socket.cpp` compiles and links (`ws2_32`)
   clean through the real Gecko/`moz.build` toolchain integration.
2. A standalone MSVC `cl` compile of the same production sources against
   the real `<winsock2.h>`/`<ws2tcpip.h>` (not a mock).
3. `SocketSmokeTest.cpp`, rebuilt and run — confirms real network
   behavior: a genuine loopback TCP connection, byte-accurate echo, and
   a real connection-refused failure path.

No sandbox-only claim remains for `Socket`/`IpAddress`. `IpAddress`
itself was always pure-logic/no-OS-dependency and fully sandbox-verified
from the start; it's `Socket`'s Winsock-facing half that needed all
three legs above. Phase 5, in its entirety, is done.

## Phase 6 — Runtime Integration — DONE, real-build-confirmed and benchmarked (2026-07-30)

Replaced the prototype pieces living directly in `forge/forge.cpp`
(`std::vector<std::unique_ptr<Microtask>>`, the linear-scan
`std::vector<std::unique_ptr<JsTimer>>` timer registry, `std::ifstream`/
`std::stringstream` script loading) with Forge Core equivalents:

- Microtask queue: `forge::core::Queue<memory::UniquePtr<Microtask>>`.
- Timer registry: `forge::core::HashMap<int, memory::UniquePtr<JsTimer>>`,
  keyed by the JS-visible `jsId`.
- Script loading: `Path::Create(StringView(argv[1]))` +
  `File::ReadAllText(path)` instead of raw file streams.
- Allocation: `EnqueueMicrotask`/`SetTimeout`/`SetInterval` now go through
  `memory::MakeUnique<T>(cx)` (`Result<UniquePtr<T>>`), reporting
  `JS_ReportOutOfMemory(cx)` on failure instead of `std::make_unique`'s
  silent `std::terminate()` under this project's `-fno-exceptions` build.
- `Runtime::Initialize()` changed from `[[nodiscard]] bool` to
  `[[nodiscard]] Result<void>`, surfacing `Error().NativeCode()` on
  failure at the call site in `main()`.

Two real bugs were found and fixed during this rewrite (see HISTORY.md's
"Phase 6 corrected" entry for the full detail):

1. A GC-root-tracing gap — `Queue<T>` had no way to walk every pending
   element (only `Front()`/`Back()`), so `TraceForgeRoots` could only
   trace the front microtask's callback, not every still-queued one.
   Fixed by adding `Queue<T>::operator[]` (front-relative indexed access).
2. A use-after-free-on-OOM-rollback bug in the timer registry's `Add()`:
   it scheduled the native timer before knowing whether the JS-side
   wrapper could actually be stored; if `HashMap::Insert` failed after
   the timer was already armed, the timer would remain scheduled against
   memory about to be freed. Fixed by cancelling the native timer
   explicitly on that failure path.

**Verification status**: `forge.cpp` itself cannot be compiled in the
sandbox this was written in (no SpiderMonkey/Gecko toolchain, wrong OS).
Every container/allocator API call used above (`Queue::Push/Front/Pop/
operator[]`, `HashMap::Insert/Find/Erase`, iteration, `MakeUnique<T>`, the
OOM-rollback path) was verified by compiling a harness directly against
the real `forge-core/Queue.h`, `HashMap.h`, `Result.h`, `memory/
MakeUnique.h`, `memory/UniquePtr.h` staged live off the device (not
reimplemented mocks), under g++/clang++ with full warnings, clean under
ASan+UBSan, and clean under valgrind (0 leaks). That was strong evidence
the container usage was correct, but was not itself a real-build
confirmation.

**Now real-build-confirmed**: the user built this exact `forge.cpp` with
a real `mach build` and ran the full `bench/run-benchmarks.ps1` suite
against it on 2026-07-30. Two follow-up fixes were needed before every
benchmark script could complete cleanly (see below); after both, all six
scripts in the bench suite ran successfully end to end. See
`HISTORY.md`'s "Phase 6 benchmarked" entry for the two real bugs the
stale-binary investigation surfaced along the way (`C:\Forge\bin\
forge.exe` was not the freshly built binary — `build.bat`/`install.ps1`
only copy from `obj-spider\dist\bin\forge.exe`, and that copy step had
been skipped), and the final same-machine Forge/Bun/Node comparison
across all six benchmark scripts, which is folded into
`Forge_Benchmark_Report.md` in full (this section intentionally does not
duplicate that table — see that report for the numbers).

Two follow-up fixes, made after the first full benchmark run surfaced
real, reproducible issues (not stale-binary artifacts — these ran
against the corrected `forge.cpp` above):

1. `microtask-bench.js` (200,000 `queueMicrotask` calls queued before any
   run) failed with a JS-level "out of memory" exception. `JS_NewContext`
   was called with an 8MB byte budget — small enough that 200,000
   simultaneously-live, uncollectable closures could plausibly exceed it.
   Raised to 512MB (Node/Bun both default to well over 1GB). Result:
   `microtask-bench.js` now completes and lands within noise of Bun
   (1.04x) and clearly ahead of Node (0.54x) — the fix resolved the
   failure as diagnosed.
2. `Microtask` carried a `JS::PersistentRootedVector<JS::Value> arguments`
   field that `ForgeJobQueue::runJobs()` never reads (it always calls
   `JS::Call` with `JS::HandleValueArray::empty()`) — removed, keeping
   `arguments` only on `JsTimer`, which genuinely needs it for
   `setTimeout`/`setInterval`'s extra-argument forwarding. This was a
   real, warranted cleanup, but measured before/after against
   `promise-chain-bench.js` (the benchmark most likely to be sensitive to
   per-job allocation overhead) showed **no measurable change** — 85.85ms
   → 85.81ms median, within run-to-run noise. So this fix should be
   credited for removing genuinely dead state, not for closing the
   promise-chain gap.

**Open item, not yet investigated**: `promise-chain-bench.js` still runs
~1.97x slower than Bun and ~1.78x slower than Node, unchanged by either
fix above. No profiling has been done to identify the actual cause —
it should not be attributed to SpiderMonkey's Promise/job-queue
machinery, or to anything else, without evidence. Next step, if this is
picked up: profile `forge.exe` while running `promise-chain-bench.js`
(a Windows profiler — e.g. Windows Performance Recorder/Analyzer, or
Visual Studio's built-in profiler — attached to a real build) to find
where the time actually goes, rather than guessing further.

## Phase 7 — Filesystem JS API — DONE: 7.1/7.2/7.3 real-build-confirmed; 7.4 (benchmarks) run and reported (2026-07-30); 7.5 (Async fs) in progress, see below

Chosen over three other candidates considered at the same time
(networking bindings, worker-thread bindings, a minimal module system) as
the highest-impact, lowest-dependency-risk next milestone: `File`/`Path`
(Phase 3) are already fully implemented and real-build-confirmed, sitting
completely unreachable from JavaScript today — `forge.cpp` defines
exactly five JS-visible functions (`print`, `setTimeout`, `setInterval`,
`clearTimeout`, `queueMicrotask`), none of them touching the filesystem.
Networking was ruled out for this phase specifically because `Socket.cpp`
isn't even in `moz.build`'s `SOURCES` yet (see the Phase 5 entry above) —
real-build-confirming that is a prerequisite this phase doesn't need.
Worker threads were ruled out as the heaviest design risk (a proper
JS-realm-per-thread story, not just a binding problem) and `ThreadPool`
also isn't real-build-confirmed yet. A module system was ruled out as a
green-field JS-semantics design problem (resolution algorithm, caching,
CommonJS-vs-ESM) rather than exposing already-built native capability.

**This phase is explicitly split into a design stage and an
implementation stage, and implementation must not start until the design
stage is reviewed and frozen by the user.**

### 7.1 — API design (this entry's deliverable; design only, nothing implemented)

Two frozen-candidate spec docs, following this project's existing
`Path.md`/`File.md`/`Socket.md` shape (Purpose, Responsibilities,
Non-Goals, Design Goals, Public API, Ownership, Error Handling Policy,
Thread Safety, Dependencies, Extensibility, Acceptance Criteria,
Implementation Status):

- **`JsBindings.md`** — the reusable conventions layer every future
  JS-visible binding (fs now; net/threads later) builds on: how a
  `Result<T>`/`Result<void>` failure becomes a thrown JS `Error` with a
  `.code` matching `ErrorCode` (plus `.syscall`/`.path`/`.nativeCode`
  where applicable), JS string ↔ `String`/`Path` marshalling, and native
  bytes ↔ `Uint8Array`/`ArrayBuffer` marshalling. Also fixes the
  JS-visible naming convention going forward: namespaced globals
  (`globalThis.fs`, later `globalThis.net`/`globalThis.threads`) rather
  than more flat functions, and a `*Sync` suffix reserved from the start
  so a later Promise-returning `fs.readFile` isn't a breaking rename.
- **`Fs.md`** — the concrete `globalThis.fs` surface for this phase:
  `readFileSync`, `writeFileSync`, `appendFileSync`, `existsSync`,
  `mkdirSync`, `rmSync`, `statSync` (size only) — each a direct,
  no-new-forge-core-behavior composition of existing `File`/`Path`
  calls. Explicitly out of scope and documented as such rather than
  invented: async I/O (7.5, gated on `ThreadPool`), directory removal
  (`File::Remove` is `DeleteFileW`-backed — files only, cannot remove a
  directory at all), directory listing, a `fs.Stats`-shaped object with
  `isFile`/`isDirectory`/timestamps (`File.h` has no type-query or mtime
  API to back it), streaming, symlinks/permissions/locking.

Both docs are delivered alongside this `ROADMAP.md` entry, at
`forge/JsBindings.md` and `forge/Fs.md`. **Nothing past this point is
implemented yet.**

### 7.2 — JS/native marshalling primitives — DONE, fully real-build-confirmed (2026-07-30)

Implements `JsBindings.md`'s six helper functions
(`ThrowJsError`/`ToForgeString`/`ToForgePath`/`FromForgeString`/
`Uint8ArrayFromBytes`/`AsByteSpan`) in `forge.cpp`, immediately after
`QueueMicrotask` and before `main()`. All six are `static` (internal
linkage), matching every other helper in the file.

**`python mach build` succeeded against these six helpers (2026-07-30) —
that part of Phase 7.2 is real-build-confirmed, not just
header-verified.** Two purely-additive follow-ups were made *after* that
successful build:

- **Linkage cleanup**, marking all six `static` and dropping the
  now-inaccurate `[[maybe_unused]]` markers (harmless either way — see
  `HISTORY.md` for why the markers were inconsistent to begin with).
- **A smoke test suite** (`RunMarshallingSmokeTests`, run via
  `forge --self-test`), added right after the six helpers: it builds a
  small in-memory JS expression via the same
  `JS::SourceText`/`JS::Evaluate` machinery `main()` already uses for
  real script files, then calls each helper against a live
  `JSContext`/`Realm` and checks the result. 12 cases: both
  `ToForgeString` success paths (string literal, number), its
  Symbol-input failure path (verifies `JS::ToString`'s TypeError
  propagates as `InvalidArgument` with no exception left pending — the
  `JS_ClearPendingException` contract below), `ToForgePath`,
  `FromForgeString`'s round-trip, `Uint8ArrayFromBytes`'s content and
  `JS_IsUint8Array`/byte-length checks, `AsByteSpan` against a real
  `Uint8Array` and a real `ArrayBuffer`, `AsByteSpan` rejecting a
  `Float64Array` and a plain number, and `ThrowJsError`'s
  thrown-exception shape (`.code`/`.syscall`/`.path` for a `NotFound`;
  `.code`/`.nativeCode` for a `PlatformError`).

**Both follow-ups are now also real-build-confirmed (2026-07-30):** a
second `python mach build` (including the linkage cleanup and the smoke
test suite) succeeded, and `forge --self-test` printed
`[self-test] ALL PASSED` with exit code 0 (all 12 cases). Phase 7.2 as a
whole — the six helpers, the linkage cleanup, and the smoke test suite —
is fully real-build-confirmed; no part of it remains sandbox-only.

The binary-marshalling question flagged as this milestone's main unknown
was resolved as: copy into a freshly-allocated `Uint8Array` via
`JS_NewUint8Array` + `JS_GetUint8ArrayData` + `memcpy` (not
`JS::NewArrayBufferWithContents`/`JS::NewExternalArrayBuffer`) — simpler
and sufficient here since `forge-core`'s `Vector<u8>` isn't allocated
compatibly with `JS_free`/a custom `BufferContentsDeleter` the way those
two APIs require, and nothing in `Fs.md`'s surface needs a zero-copy
hand-off in the native→JS direction. The JS→native direction
(`AsByteSpan`) *is* zero-copy, aliasing the JS buffer's own storage for
the duration of the call, as designed.

**Verification method, and its limit (read this before treating 7.2 as
done):** every SpiderMonkey function/type used
(`JS_NewStringCopyUTF8N`, `JS_IsUint8Array`, `JS_NewUint8Array`,
`JS_GetUint8ArrayData`, `JS_GetTypedArrayByteLength`,
`JS::IsArrayBufferObject`, `JS::IsDetachedArrayBufferObject`,
`JS::GetArrayBufferData`, `JS::GetArrayBufferByteLength`,
`JS::AutoCheckCannotGC`, the `Rooted`/`Handle`/`MutableHandle` implicit-
conversion rules relied on) was confirmed by reading the real headers in
this tree's own `js/public/` directly (`ArrayBuffer.h`,
`experimental/TypedData.h`, `String.h`, `CharacterEncoding.h`,
`Exception.h`, `ErrorReport.h`, `Conversions.h`, `PropertyAndElement.h`,
`Value.h`, `GCAPI.h`, `RootingAPI.h`) — not guessed or reconstructed from
memory. Separately, the forge-core-facing logic (`Result<T>`/`Failure`
construction, `String::Create`, `Path::Create`, `Vector<u8>`, `Span<u8>`,
`StringView` conversions) was compiled and run against the real
`forge-core` headers plus a signature-faithful fake JSAPI shim, under
g++ and clang++ (`-std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic
-Werror`), ASan+UBSan, and valgrind (16/16 scenarios pass; 0 leaks, 0
errors) — 16 cases covering every branch (`ToString`/`EncodeStringToUTF8`
failure, OOM, `Uint8Array`/`ArrayBuffer`/detached/shared/non-object
inputs, `ThrowJsError`'s pending/non-pending/`PlatformError` paths).

What this sandbox verification could not cover on its own — an actual
compile against the real `js/public` headers, which needs the full
mfbt/mozilla-central build graph unavailable here (same limitation
`File.md` already documents for its own Win32 code) — is exactly what the
real `python mach build` above closed. Between the sandbox pass (real
API names, real forge-core allocation/`Result<T>` behavior) and the real
build (real SpiderMonkey compile) and the smoke test suite (real
`JSContext` exercising every branch), this milestone has no remaining
unverified surface.

No design deviation from the frozen `JsBindings.md`/`Fs.md` specs was
needed. One behavioral clarification (not a signature change) was added
to `JsBindings.md`'s Error Handling Policy alongside this implementation:
`ToForgeString`/`ToForgePath` never leave their own exception pending on
`cx` on a `Result<T>` failure (`JS_ClearPendingException` internally,
where needed), so every caller can uniformly go through `ThrowJsError` on
any `Result<T>::HasError()` without a double-pending-exception risk.

### 7.3 — Synchronous `fs` bindings — IMPLEMENTED, sandbox-verified, pending real build (2026-07-30)

Implements every method in `Fs.md`'s Public API
(`readFileSync`/`writeFileSync`/`appendFileSync`/`existsSync`/
`mkdirSync`/`rmSync`/`statSync`) in `forge.cpp`, immediately after the
Phase 7.2 smoke tests and before `main()`, wired to `globalThis.fs` via
`DefineFsNamespace` (a plain object populated with `JS_DefineFunction`,
then attached to the global via `JS_DefineProperty`) the way `Runtime`
already wires up `print`/timers today. Every method composes existing
`File`/`Path` calls exactly per spec — no new `forge-core` behavior.
`WriteAllBytes` implements the write-retry loop `Fs.md` requires (`Fs.md`:
"a short write ... is retried by the binding until the full buffer is
written or an error occurs"); `ResolveWriteData` resolves
`writeFileSync`/`appendFileSync`'s `data` argument (string, `Uint8Array`,
or `ArrayBuffer`) to a byte span, UTF-8-encoding a string via the same
`ToForgeString` helper Phase 7.2 already proved.

A 17-case `forge --self-test` smoke suite (`RunFsSmokeTests`, appended to
the same `--self-test` command Phase 7.2's suite already uses) covers
every method with at least one success path and one failure path, per
`Fs.md`'s own Acceptance Criteria — each case is a small JS snippet
(calling `fs.*Sync` through the real, namespaced global exactly as a
real script would) run via the same `EvaluateExpression` machinery
Phase 7.2's suite already established. Fixtures are deterministic,
fixed-name paths under a `forge-selftest-fs-scratch/` scratch directory
(no `Date.now()`/`Math.random()`) so repeated runs don't accumulate
garbage; the one non-idempotent case (`mkdirSync` without `recursive` on
an already-existing directory throws `AlreadyExists` on a second run) is
handled by the test itself tolerating that specific, documented outcome
rather than treating it as a failure.

**A real fix made in the same pass, not flagged by the user:**
`RunMarshallingSmokeTests` used to print its own
`[self-test] ALL PASSED`/`FAILURES ABOVE` summary line internally. Now
that `--self-test` runs both suites, that would have printed a
possibly-false "ALL PASSED" before the fs suite (which runs afterward)
had a chance to fail — fixed by moving the combined summary print to
`main()`, after both `RunMarshallingSmokeTests` and `RunFsSmokeTests`
have run.

**Verification status — fully real-build-confirmed (2026-07-30).** The
user ran `python mach build` against this exact `forge.cpp` and it
succeeded; `forge --self-test` printed `[self-test] ALL PASSED` across
all 29 cases (the original 12 Phase 7.2 cases plus these 17), exit code
0. Phase 7.3 — all seven `fs.*Sync` bindings and their smoke test suite —
is real-build-confirmed, not just sandbox-verified. Before that real
build, sandbox verification (described below for the record) had already
covered the binding logic two other ways, since this phase's own
`RunFsSmokeTests` (real `JS::Evaluate` parsing multi-statement JS, real
`fs.*Sync` property-based dispatch, real Win32 `File` I/O) could not be
run in the sandbox at all — the fake JSAPI shim used there only pattern-
matches Phase 7.2's simple literal-value expressions, not Phase 7.3's
snippets that call into a real bound method:

1. **Compile check.** The entire new `forge.cpp` section (all seven
   binding functions, `ResolveWriteData`, `WriteAllBytes`,
   `DefineFsNamespace`, and all 17 self-test cases plus
   `RunFsSmokeTests`) was compiled against the real `forge-core` headers
   and an extended fake JSAPI shim (adding `JS_NewPlainObject`, the
   `JS::Handle<JSObject*>`/`double`-value `JS_DefineProperty` overloads,
   `JSPROP_ENUMERATE`, `JS::ToBoolean`, and a real `JS::CallArgsFromVp`-
   compatible `JS::CallArgs`) under g++ and clang++ with
   `-std=c++17 -Wall -Wextra -Wpedantic -Werror` — 0 warnings, 0 errors.
   This caught one real bug before it reached the user: an unused
   `JSContext* cx` parameter on `SelfTest_FsEnsureScratchDir` (the
   fixture-setup helper doesn't need one) that would have failed the
   real build under `-Werror` — fixed by dropping the parameter.
2. **Direct-`CallArgs` logic verification.** Since the fake shim can't
   run the production JS snippets, a separate sandbox-only harness
   invokes all seven `Fs*Sync` functions directly (constructing a
   `JS::Value* vp` array matching the real engine's calling convention,
   the same way a real embedder would call a `JSNative` without going
   through script evaluation), against a POSIX (not Win32)
   re-implementation of `File`'s backend written for this harness only
   (never delivered) — real file I/O on Linux, exercising the actual
   binding logic end-to-end. 20/20 checks pass, covering every method's
   success and failure path (including `writeFileSync`'s `Uint8Array`
   and invalid-data cases, `mkdirSync`'s recursive/non-recursive/missing-
   parent cases, and `statSync`'s size and `NotFound` cases), plus
   `DefineFsNamespace` itself. Re-ran the full Phase 7.2 suite in the
   same binary as a regression check: still 12/12. All of the above also
   passed clean under ASan+UBSan (0 errors with `detect_leaks=0`; leaks
   with detection on trace entirely to the fake shim's own intentional
   non-freeing of simulated engine objects, same as Phase 7.2's own
   finding) and valgrind memcheck (`ERROR SUMMARY: 0 errors from 0
   contexts`).
3. **What neither of the above covered on its own:** the real Win32
   `File` backend and real `JS::Evaluate`/property-based `fs.xxx()`
   dispatch through an actual SpiderMonkey engine — exactly what the real
   `mach build` + `forge --self-test` run above closed.

No design deviation from the frozen `Fs.md`/`JsBindings.md` specs was
needed. One implementation-level judgment call, not a spec deviation:
`existsSync` still throws on a genuine `File::Exists` failure rather than
swallowing every error and returning `false` the way Node's own
`fs.existsSync` does — `Fs.md`'s Error Handling Policy is unconditional
("every `Result<T>`/`Result<void>` failure throws") and its Design Goals
explicitly permit divergence from Node's exact behavior wherever
`forge-core`'s own semantics differ, which they do here (`forge-core`
draws a real distinction between "doesn't exist" and "couldn't tell").

### 7.4 — Benchmarks + docs (Small complexity; depends on 7.3) — benchmark run complete (2026-07-30)

`bench/fs-bench.js` written (write/read round-trip loop reusing one file
across all iterations, plus a lighter pass over `appendFileSync`/
`existsSync`/`mkdirSync`/`rmSync`/`statSync` so the whole `Fs.md` surface
gets touched at least once) and added to `run-benchmarks.ps1`'s
`$Benchmarks` array, closing the "filesystem operations" gap every
benchmark report since Phase 0 has had to flag as not-yet-benchmarkable.
That script's own stale header comment (claiming "there is deliberately
no filesystem... benchmark here") was corrected at the same time.

**Real-build run, same-machine, same methodology as every other script**
(user ran the updated `run-benchmarks.ps1` against `C:\Forge\bin\
forge.exe`, `bun.exe`, and `node.exe`, default 5 iterations,
2026-07-30). All seven scripts in the suite — including the
timer/microtask/promise-chain batch that `run-benchmarks.ps1`'s own
header comment had flagged as "not yet run end-to-end" — completed
successfully on all three runtimes with no `FAILED` rows, confirming
(for the first time) that the full current bench suite actually runs
clean against this build. `fs-bench.js` itself: forge/bun ratio =
**0.86x**, forge/node ratio = **0.91x** (medians 864.01ms forge vs.
999.30ms bun vs. 944.82ms node, 2000 round-trip iterations + 100
extra-pass iterations) — Forge is faster than both on this benchmark.
Full per-script table and CSV: `bench/results/2026-07-30_175703.csv`.

**Resolved:** this doc, `HISTORY.md`, and `PROJECT_CONTEXT.md` all
referenced a `Forge_Benchmark_Report.md` as an existing artifact holding
the full same-machine Phase 6 comparison table, but a direct listing of
the live `js/src/forge` directory on 2026-07-30 showed no such file
existed there. Raised with the user directly rather than silently
resolved either way; the user asked for a fresh report to be authored
from today's real run. `Forge_Benchmark_Report.md` now exists, dated
2026-07-30, built from the same-machine run above (all seven scripts,
not just `fs-bench.js`), and its own Provenance Note section documents
this discrepancy explicitly rather than presenting itself as a recovered
copy of whatever the earlier reference pointed to. Phase 7.4, and Phase
7 as a whole, is now DONE.

### 7.5 — Async `fs` — design frozen (2026-08-02); implementation in progress

Promise-returning `fs.readFile`/`fs.writeFile`/`fs.appendFile`/
`fs.exists`/`fs.mkdir`/`fs.rm`/`fs.stat`, dispatched onto `ThreadPool`
(Phase 4, real-build-confirmed) so file I/O no longer blocks the single
event-loop thread. Originally scoped out of Phase 7 as a stretch item
gated on `ThreadPool` being real-build-confirmed — it now is (see the
Phase 4 entry above) — and was chosen over three other next-milestone
candidates (Networking `net.*` bindings, worker-thread bindings, a
minimal module system) as the highest-impact option that builds directly
on already-verified foundations rather than opening a large new design
problem. The `promise-chain-bench.js` performance gap (Forge ~2x slower
than Bun/Node) is explicitly kept out of this phase — it's a separate
profiling task requiring real profiling data, not something to guess at
here.

**This phase follows the same design-stage/implementation-stage split
Phase 7 itself used: implementation must not start until the design is
reviewed and frozen by the user.**

#### 7.5.0 — Design — FROZEN (2026-08-02)

`FsAsync.md`, delivered and reviewed, following `JsBindings.md`/`Fs.md`'s
established section shape. Grounded in direct reads of `IocpLoop.h`,
`ThreadPool.h`, `forge.cpp`'s `Runtime` class, and `IocpSmokeTest.cpp`
(not assumption), and settles:

- **The thread boundary is absolute**: a `ThreadPool` worker touches only
  `Path`/`Vector<u8>`/`File`/`Result<T>` — never `JSContext`, `JS::Value`,
  or anything JSAPI-typed. All JS-facing work (argument parsing, result
  marshalling, `Promise` resolution/rejection) happens on the main thread.
- **Write/append data is copied, not aliased, before dispatch** — the
  existing sync-path `AsByteSpan`/`ResolveWriteData` aliasing contract
  (valid only "for the duration of the call", per `JsBindings.md`) cannot
  survive a hand-off to a worker thread that runs at some later, arbitrary
  time.
- **The returned `Promise` is GC-rooted independent of script** (a
  `JS::Heap<JSObject*>` traced by `TraceForgeRoots`, matching `Microtask`/
  `JsTimer`'s existing pattern since Phase 6) — a fire-and-forget
  `fs.writeFile(path, data);` with no `.then()` must still complete
  correctly.
- **`Runtime::run()`'s event-loop exit condition has a real, pre-existing
  gap** (`jobQueue_.empty() && loop_.Empty()` doesn't account for
  in-flight async fs operations — `IocpLoop::Empty()`'s own doc comment
  already predicted this) that this phase must close by adding a
  pending-operation counter to `Runtime` itself (not `IocpLoop`, which
  stays frozen), extending the exit check to
  `... && pendingAsyncOps_ == 0`.
- **Cross-thread `PostCompletion` is the reused wakeup mechanism, but has
  never actually been tested** — `IocpSmokeTest.cpp`'s only real evidence
  is a same-thread post-then-`RunOnce()` pattern, never a genuine
  cross-thread wakeup. Flagged as this phase's single biggest open
  technical risk, with a dedicated real-machine smoke test required before
  the phase can be considered done (see 7.5.1 below).
- **Native `JS::Promise` construction/resolution is new binding surface**
  — confirmed via grep that no `JS::NewPromiseObject`/`ResolvePromise`/
  `RejectPromise` call exists anywhere in this codebase yet.
- No callback-based `fs.promises`-style split (unlike Node) — `JsBindings.md`
  already settled that the bare name is the Promise-returning variant.
  No cancellation, no streaming, no ordering guarantees across concurrent
  calls, no change to `fs.*Sync`/`JsBindings.md` (both frozen, depended on
  not modified), no change to `IocpLoop`'s public API.

User reviewed `FsAsync.md` on 2026-08-02 and explicitly confirmed: the
worker/main-thread split, the copy-before-dispatch rule, the
pending-operation counter and event-loop lifetime fix, and the two new
required tests (cross-thread `PostCompletion` smoke test,
`Promise`-based self-tests) are all part of this phase's commitment.
**Design frozen. Nothing past this point was implemented before the
freeze; implementation begins now, incrementally, each milestone verified
by a real `python mach build` per the user's stated workflow.**

#### 7.5.1 — Cross-thread `PostCompletion` smoke test — DONE, real-machine-confirmed (2026-08-02)

The design's single biggest open risk, addressed first, before anything
else in this phase is built on top of the assumption that it works.
`IocpCrossThreadSmokeTest.cpp` (new file, delivered alongside this entry,
same "separate console-app project, not part of `moz.build`" pattern as
`IocpSmokeTest.cpp`/`ThreadingSmokeTest.cpp`/`SocketSmokeTest.cpp`):

- Initializes a real `IocpLoop`, schedules a 5-second non-repeating
  safety-net timer (so a genuine failure of the cross-thread pattern
  produces a bounded-time, clearly-reported FAILURE rather than an
  indefinite hang), then spawns a real `Thread` (Phase 4) that sleeps
  200ms and calls `PostCompletion` — from that thread, not the main one.
- The main thread calls `RunOnce()` with no work already posted, so it
  genuinely blocks inside `GetQueuedCompletionStatus`'s wait — exactly the
  cross-thread pattern `IocpSmokeTest.cpp` never exercised.
- Deliberately avoids any shared-state mutation from the worker thread
  that isn't synchronized via `Thread::Join()` afterward (a plain
  `workerPostFailed` bool, not the shared `CHECK`-driving `g_failures`),
  since nothing else synchronizes the worker's post-`PostCompletion`
  statements with the main thread's own checks around `RunOnce()`
  returning — the IOCP completion handoff itself only guarantees ordering
  up to the `PostCompletion` call, not after it.

**Real-machine-confirmed (2026-08-02).** The user's standalone MSVC build
needed two source files added beyond this file's own original build
comment — `forge-core/platform/IocpLoop.cpp` and
`forge-core/platform/TimerScheduler.cpp` — since `IoLoop.h` only pulls in
`IocpLoop`'s (and, transitively, `TimerScheduler`'s) declarations, not
their definitions; the linker failed on unresolved `IocpLoop::*`/
`TimerScheduler::*` symbols without them. The build comment has been
corrected accordingly. Once fixed, the compile succeeded and the test
ran clean:

```
IocpCrossThreadSmokeTest: loop initialized
IocpCrossThreadSmokeTest: worker thread posting completion after delay...
IocpCrossThreadSmokeTest: RunOnce() woke up (bytes=99, key=7, ok=1)
IocpCrossThreadSmokeTest: all checks passed
```

This confirms, on the real machine, that a `PostCompletion` call from a
genuinely different OS thread does wake a thread blocked inside
`RunOnce()`'s `GetQueuedCompletionStatus` wait (the test's 5-second
safety-net timer did not fire — the completion itself woke the loop).
The biggest open risk this phase's design flagged is now closed. 7.5.2
can start.

#### 7.5.2 — `Runtime` pending-operation counter + event-loop lifetime fix — DONE, real-build-confirmed (2026-08-02)

Adds `forge::core::Size pendingAsyncOps_{ 0 }` to `Runtime` (`forge.cpp`),
plus two new public methods:

- `OnAsyncOperationStarted()` — increments the counter; called (via
  `GetRuntime(cx)`, the existing accessor pattern `timers()` already
  uses) at the moment a future async fs operation is dispatched onto
  `ThreadPool` (7.5.3 onward).
- `OnAsyncOperationFinished()` — decrements the counter; called once that
  operation's completion has been delivered and its `Promise` settled.
  Asserts (`FORGE_ASSERT`, `forge-core/Assert.h`, newly included in
  `forge.cpp`) that the count is nonzero before decrementing — a
  mismatched Started()/Finished() pair is a real programmer error, not a
  tolerated edge case, matching the "loud failure over silent
  misbehavior" convention `Thread.h`'s `Joinable()`/destructor contract
  already uses elsewhere in this codebase.

`run()`'s exit condition is extended from `jobQueue_.empty() &&
loop_.Empty()` to `jobQueue_.empty() && loop_.Empty() &&
pendingAsyncOps_ == 0`, closing the gap `IocpLoop::Empty()`'s own doc
comment predicted and `FsAsync.md`'s Design Goals called out. `IocpLoop`
itself is untouched — the fix lives entirely in `Runtime`, per the
design's Dependencies section.

**No behavior change is possible yet, by construction**: nothing in the
codebase calls `OnAsyncOperationStarted()` anywhere until 7.5.3 adds a
real caller, so `pendingAsyncOps_` is provably always `0` today and the
new exit-condition term is always true — every existing script and the
full `forge --self-test` suite (29 cases) should behave identically to
before this change. That provable inertness is this milestone's own
verification story: a real `python mach build` succeeding, plus
`forge --self-test` still printing `[self-test] ALL PASSED` unchanged,
confirms the addition compiles and introduces no regression, without
needing a synthetic "manually toggle the counter" test that would have
to be deleted again once 7.5.3 gives it a genuine caller.

**Real-build-confirmed (2026-08-02).** The user's `python mach build`
succeeded, and `forge --self-test` printed `[self-test] ALL PASSED`
across all 29 cases, unchanged — exactly the "provably inert" outcome
predicted above. 7.5.3 can start.

#### 7.5.3 — Worker dispatch + `Promise` settlement machinery, `fs.readFile` first — DONE, real-build-confirmed (2026-08-02)

`AsyncFsReadOp` (`forge.cpp`, defined just before `Runtime` so `Runtime`
can hold it as a complete-type `HashMap` member without any
incomplete-type risk): embeds `IoCompletion` first (per its documented
zero-vtable-call idiom), a GC-rooted `JS::Heap<JSObject*> promise`, an
owned `Path`, and two placeholder-initialized `Result<T>` fields
(`bytesResult`/`textResult`, matching `readFileSync`'s own binary-vs-utf8
split) that the worker thread writes and the completion callback reads.

`Runtime` gains: a shared `ThreadPool workerPool_` (4 workers, sized the
same as `ThreadingSmokeTest.cpp`'s own real-machine-confirmed case, not
tuned against a real workload -- that's 7.5.6), a
`HashMap<AsyncFsReadOp*, UniquePtr<AsyncFsReadOp>> pendingFsReadOps_`
(owns every in-flight op, keyed by its own address -- no script-visible
ID/cancellation exists, so unlike `JsTimerRegistry`'s int-keyed map no
lookup-by-id is needed), `RegisterAsyncFsReadOp`/`UnregisterAsyncFsReadOp`
to take/release ownership, and a `TraceRoots(trc)` method that now traces
both the timer registry's callbacks and every pending op's `Promise` --
`JS_AddExtraGCRootsTracer` is repointed to pass `this` instead of
`&timers_` so `TraceForgeRoots` can reach it. `Shutdown()` now also stops
`workerPool_` (blocking until every in-flight worker task finishes) and
then clears `pendingFsReadOps_` *while `cx` is still alive* -- releasing
each entry's `JS::Heap<promise>` in a context where its own GC-barrier
machinery still has a live context to work with, rather than leaving that
to `Runtime`'s implicit destructor, which runs later in `main()`, after
`JS_DestroyContext(cx)`.

`fs.readFile(path, encoding?)`: parses arguments identically to
`readFileSync`, then creates the `Promise` with a null executor (the
first native `JS::NewPromiseObject` call in this codebase -- its exact
signature, along with `JS::ResolvePromise`/`JS::RejectPromise`, was
confirmed by fetching the real `js/public/Promise.h` from this same
`gecko-dev` tree on GitHub, mirroring Phase 7.2's `JS_NewPlainObject`
precedent of reading the real declaration rather than reconstructing one
from memory -- quoted verbatim in `forge.cpp`'s own comment above the
new section). Registers the op, then submits a worker lambda that
captures only `op` (a raw pointer -- the worker touches only its
`path`/`wantUtf8`/`bytesResult`/`textResult` fields, all forge-core
types) and a raw `IoLoop*` (only to call `PostCompletion`, now
real-machine-confirmed by 7.5.1). The worker never touches `cx`,
`op->promise`, or anything JSAPI-typed -- the absolute thread-boundary
rule `FsAsync.md`'s Design Goals require. `FsAsyncReadOpCompleted` (the
`IoCompletion` callback, dispatched by `RunOnce()` on the main thread)
resolves or rejects the `Promise` from the worker's stored `Result<T>`,
reusing `ThrowJsError` for the rejection shape (a small
`RejectAsyncFsPromise` helper throws via `ThrowJsError`, retrieves the
resulting exception as a value, clears it, and hands it to
`JS::RejectPromise` -- a rejection value must never also be a pending
exception on `cx`), then calls `OnAsyncOperationFinished()` and
`UnregisterAsyncFsReadOp()` last.

**Known, accepted gaps, disclosed rather than silently assumed away:**
if `ThreadPool::Submit`'s own internal `PostCompletion` call somehow
fails (the completion port itself invalid), there is no way to report
that from the worker thread, and the op's `Promise` never settles --
an exceptional, not routine, failure mode this milestone does not solve.
If `run()` exits early (a genuine I/O error) while a read is still in
flight, `Shutdown()` now safely drops that op's unsettled `Promise`
rather than leaving a use-after-free risk, but the operation's actual
result (if it ever would have completed) is simply lost -- no script is
left running to observe it either way once `run()` has exited, so this
is disclosed as harmless-but-lossy, not fixed.

A manual verification script, `manual-test-fs-async.js` (delivered
alongside this entry), exercises both encodings' success paths and the
`NotFound` rejection path, and -- since its only pending work once its
body finishes is these three `fs.readFile()` calls, no timers anywhere
in the file -- also exercises the Phase 7.5.2 event-loop lifetime fix
directly: without `pendingAsyncOps_`, Forge would exit before any of
these reads completed. Run via `forge manual-test-fs-async.js`. This is
explicitly a stand-in for Phase 7.5.5's real self-test harness (today's
`--self-test` suites call binding functions directly and never let
`Runtime::run()` actually drive the event loop, so they cannot exercise
a real async completion yet), not a replacement for it.

**Real-build-confirmed (2026-08-02).** `python mach build` succeeded, and
`manual-test-fs-async.js` printed:

```
PASS: fs.readFile rejected with .code === "NotFound" for a missing file
PASS: fs.readFile(path, "utf8") resolved with the correct text
PASS: fs.readFile(path) resolved with a Uint8Array of the right length
```

Confirms, on the real machine: the first native `JS::NewPromiseObject`/
`ResolvePromise`/`RejectPromise` calls in this codebase work correctly
for both the resolve and reject paths; both `readFile` encodings
(`utf8` string, default `Uint8Array`) marshal correctly from a
worker-thread `Result<T>` into the right JS value; the rejection shape
(`.code`) round-trips through `ThrowJsError`'s reuse correctly; and the
process waited for all three in-flight reads to complete before exiting
— the Phase 7.5.2 event-loop lifetime fix, exercised for the first time
against a genuine async fs operation rather than proven only "inert" by
construction. 7.5.4 (the remaining six methods) can start.

#### 7.5.4 — Remaining six methods — DONE, real-build-confirmed (2026-08-02)

`fs.writeFile`/`fs.appendFile`/`fs.exists`/`fs.mkdir`/`fs.rm`/`fs.stat`,
sharing one new bookkeeping struct (`AsyncFsMiscOp`) and one dispatch
helper (`DispatchAsyncFsMiscOp`) rather than repeating `fs.readFile`'s
whole struct-plus-dispatch shape six times — a `kind` enum discriminant
selects which of three placeholder-initialized `Result<T>` fields
(`voidResult`/`existsResult`/`statResult`) is authoritative once the
worker has run, generalizing the exact pattern `AsyncFsReadOp`'s own
`bytesResult`/`textResult` already established for its two encodings.
`Runtime` gains a second `HashMap<AsyncFsMiscOp*, UniquePtr<AsyncFsMiscOp>>`
(`pendingFsMiscOps_`) alongside 7.5.3's read-op map; `TraceRoots`/
`Shutdown` were extended to cover both maps identically.

`writeFile`/`appendFile` exercise the copy-before-dispatch rule (Design
Goals) for the first time: a new `CopySpanToVector` helper copies
`ResolveWriteData`'s resolved span into an owned `Vector<u8>`
synchronously, on the main thread, before dispatch — `ResolveWriteData`'s
returned span may alias the caller's own JS buffer or its own
`ownedText` storage, neither safe to hand across the thread boundary to
a worker that runs at some later, arbitrary time. A new
`WriteAllBytesNoThrow` (a worker-thread-safe variant of the existing
`WriteAllBytes`, returning `Result<void>` instead of calling
`ThrowJsError`) runs the actual short-write-retry loop on the worker.

`mkdir`/`rm`/`exists`/`stat` each reuse their sync counterpart's exact
`File::*` call and semantics (including `mkdir`'s `options.recursive`
parsing and `exists`'s Node-divergence rationale), just dispatched
through the shared worker path instead of running inline.

`manual-test-fs-async.js` (the same file from 7.5.3, extended) now
chains `writeFile → appendFile → stat → exists → rm → exists → mkdir
(recursive) → mkdir (non-recursive, expects AlreadyExists)` sequentially
through `.then()`, rather than firing concurrently — two async calls
racing over the same path is out of scope by design (`FsAsync.md`'s
Non-Goals: no ordering guarantee across concurrent calls), so a
sequential chain is what makes this a meaningful, non-flaky check rather
than exercising undefined interleaving.

**Real-build-confirmed (2026-08-02).** `python mach build` succeeded, and
the extended `manual-test-fs-async.js` printed all eleven PASS lines
followed by `ALL CHECKS PASSED`:

```
PASS: fs.readFile rejected with .code === "NotFound" for a missing file
PASS: fs.readFile(path) resolved with a Uint8Array of the right length
PASS: fs.readFile(path, "utf8") resolved with the correct text
PASS: fs.writeFile resolved and wrote the expected content
PASS: fs.appendFile resolved and appended the expected content
PASS: fs.stat resolved with the correct size (12)
PASS: fs.exists resolved true for a file that exists
PASS: fs.rm resolved and the file is actually gone
PASS: fs.exists resolved false for a file that no longer exists
PASS: fs.mkdir(recursive) resolved and the nested directory actually exists
PASS: fs.mkdir(non-recursive) rejected with .code === "AlreadyExists" as expected

Chained fs.* async sequence ran to completion.

ALL CHECKS PASSED
```

Confirms, on the real machine: all seven `fs.*` async methods now exist,
resolve/reject correctly, and produce the right shapes end to end,
including the sequential write→append→stat→exists→rm→exists→mkdir
(recursive)→mkdir (non-recursive, `AlreadyExists`) chain -- the
`AsyncFsMiscOp` shared-struct design, the copy-before-dispatch rule for
write/append, and the worker-thread dispatch all work correctly across
all six methods on first real build, with no bugs surfaced in this pass.
**Phase 7.5.4 is done. All seven `fs.*` async bindings specified in
`FsAsync.md`'s Public API now exist and are real-build-confirmed.**
7.5.5 (the formal Promise-based self-test harness) can start.

#### 7.5.5 — Self-tests (DONE, real-build-confirmed, 2026-08-02)

Promise-based self-tests exercising every method's success and failure
path through an actual resolved/rejected `Promise`, letting
`Runtime::run()` genuinely drive the event loop to completion — a
structurally new requirement for `forge --self-test`, whose existing
cases (Phase 7.2/7.3) all call binding functions directly and inspect a
`Result<T>` synchronously. Also includes the test confirming the
`Runtime::run()` exit-condition fix: a script whose only pending work is
one in-flight async fs call, no timers, must run to completion and exit
cleanly rather than early.

Implemented in `forge.cpp` as a new `RunFsAsyncSmokeTests(cx, runtime)`,
called from `main()`'s `--self-test` dispatch alongside (and after)
`RunMarshallingSmokeTests`/`RunFsSmokeTests`. Two groups:

- **Group A** (the full chained sequence): evaluates one script
  (`kAsyncSelfTestChainScript`) that exercises all seven `fs.*` async
  methods — the three `fs.readFile` cases fire concurrently (independent
  of each other), the six Phase 7.5.4 methods chain through `.then()`
  (each one's outcome depends on the file state the previous step left
  behind, same reasoning `manual-test-fs-async.js` documents for why
  concurrent calls over the same path would be flaky here). 12 cases
  total, each recorded via a native `globalThis.__asyncSelfTestRecord`
  callback (bound only for the duration of this suite, not part of
  `Fs.md`'s JS-visible surface) into a `Vector<AsyncSelfTestResult>`,
  since native code cannot itself await a JS `Promise` — the script
  queues every chain, then `runtime.run()` actually drives the loop to
  completion, and only then are results inspected. The harness also
  checks the *count* of recorded results (expects exactly 12), not just
  each one's `passed` field, so a chain that throws partway through and
  silently stops doesn't read identically to "zero failures."
- **Group B** (the lifetime-fix isolation probe): evaluates a second,
  separate script (`kAsyncSelfTestLifetimeProbeScript`) whose *only*
  statement is a single `fs.readFile(...).then(...)` call — no timers,
  nothing else queued — then calls `runtime.run()` again on the same
  `Runtime` (a fresh `Evaluate()`+`run()` cycle; Group A's own `run()`
  already drained everything before returning, so this starts from a
  genuinely clean job queue/`IoLoop`/`pendingAsyncOps_` state). This is
  the direct, isolated regression test for the Phase 7.5.2
  `pendingAsyncOps_` fix: without it, `run()` would see an empty job
  queue and an empty `IoLoop` and return immediately, and this `.then()`
  callback would never fire.

**Real-build-confirmed.** The user ran `python mach build` (successful)
and `forge --self-test`: all 13 expected new lines appeared and passed
— the 12 Group A cases plus Group B's lifetime-fix probe, ending in
`ALL PASSED` alongside the existing Phase 7.2/7.3 suites. No
count-mismatch line appeared, confirming the chained sequence in Group A
ran to completion and recorded exactly the 12 cases expected. Group B's
own line (`Runtime::run() waits for lone in-flight fs.readFile (no
timers)`) passed, confirming the Phase 7.5.2 `pendingAsyncOps_` fix
holds under the isolated regression shape this probe was specifically
designed to catch — not just as a byproduct of Group A's busier chain.

**Phase 7.5.5 is done. `forge --self-test` now has a genuine
Promise-based self-test harness driving `Runtime::run()` to completion,
replacing `manual-test-fs-async.js` as this feature's ongoing regression
check.** Only 7.5.6 (benchmarks + docs) remains before Phase 7.5 as a
whole is complete.

#### 7.5.6 — Benchmarks + docs (DONE, real-build-confirmed, 2026-08-02)

`bench/fs-async-bench.js` written: same iteration counts/payload as
`fs-bench.js` (Phase 7.4) for a direct comparison, chained through
`.then()` one step at a time rather than a blocking for-loop (concurrent
calls over the shared round-trip path would be a genuine race, per
FsAsync.md's own Non-Goals — see the script's header comment). Wired
into `run-benchmarks.ps1`'s `$Benchmarks` list. Verified runnable — not
against Forge (no compiler in this sandbox) but genuinely executed
against both `node` and `bun`, which caught a real bug before delivery:
neither runtime's `fs.promises` has an `.exists` method, unlike Forge's,
so the script shims one on top of `fs.promises.access()`. Both runtimes
now run the script to completion and print the identical checksum. Not
yet run against a real `forge.exe` — that's the user's real machine.

`FsAsync.md`'s `Implementation Status` section updated with the full
7.5.1-7.5.6 milestone breakdown (it had gone stale, still reading "Design
only" despite 7.5.1-7.5.5 being real-build-confirmed). `Fs.md`/
`JsBindings.md` updated at their few forward-references to "Phase 7.5" to
point at `FsAsync.md`'s now-confirmed status — neither doc's own frozen
specified behavior changed, only these pointers.

**Real benchmark run attempted (2026-08-02): blocked, not done.** The
other six pre-existing scripts reconfirmed within noise of the
2026-07-30 baseline. `fs-async-bench.js` ran to completion on Bun and
Node, but Forge (`C:\Forge\bin\forge.exe`) failed outright:
`fs.mkdir is not a function`, at the script's very first async fs.*
call. This directly contradicts the same real machine's `forge
--self-test` run (reported immediately prior, in the same conversation)
showing `fs.mkdir` passing twice as part of all 13 Phase 7.5.5 self-test
lines passing — and `forge.cpp`'s `DefineFsNamespace` registers `mkdir`
unconditionally, in the identical call path used by both `--self-test`
and any ordinary script, so there's no code-level reason for one to see
it and the other not to. Leading hypothesis (not confirmed): `C:\Forge\
bin\forge.exe` is a stale copy relative to whatever binary the
`--self-test` run actually executed. See `Forge_Benchmark_Report.md`'s
new dated section and `HISTORY.md`'s matching entry for the full
writeup.

**Resolved (2026-08-02, same day).** The stale-binary hypothesis above
was confirmed: `C:\Forge\bin\forge.exe` had not been refreshed from
`mach build`'s real output
(`C:\spidermonkey-dev\gecko-dev\obj-spider\dist\bin\forge.exe`) since
before Phase 7.5's async `fs.*` work landed. The user copied the fresh
binary over and reran — no code change was needed. The clean rerun
produced real Forge numbers for `fs-async-bench.js`:
`forge median=935.39 ms`, `forge/bun=0.75x`, `forge/node=0.66x` — Forge
faster than both, directionally consistent with `fs-bench.js`'s own
sync-path result. The other seven scripts stayed within a few percent of
the 2026-07-30 baseline. Full writeup, both runs' numbers, and a
`fs-async-bench.js`-specific analysis are in
`Forge_Benchmark_Report.md`'s "Results (2026-08-02)" section and
`HISTORY.md`'s matching entries.

**Phase 7.5.6 is done. Phase 7.5 (async `fs`) is complete end to end** —
all seven methods implemented and real-build-confirmed (7.5.1-7.5.4),
covered by a formal self-test harness (7.5.5), and now benchmarked
against Bun/Node with real numbers (7.5.6). No known open items remain
for this phase.

### Success Criteria (whole phase)

A script using only `globalThis.fs` can read a file (text and binary),
write a file, append to a file, check existence, create a (possibly
nested) directory, remove a file, and query a file's size — with every
failure surfacing as a catchable JS `Error` carrying `.code`/`.syscall`/
`.path` per `JsBindings.md` — verified by a real `mach build` running a
smoke-test script exercising both a success and a failure path per
method (same "cannot compile in this sandbox" caveat as every other
JSAPI-touching phase so far), and benchmarked against Node/Bun's `fs`
equivalents via `bench/fs-bench.js`.
