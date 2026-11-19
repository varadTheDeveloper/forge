# Phase 10: web-standard APIs

Phase 10 adds the web APIs of the WinterTC Minimum Common API (decision
D13) that need no network: [WinterTC.md](WinterTC.md) tracks every entry.
Phase 11 adds `fetch` and the rest of the network APIs.

## 1. Order

Each step lands with its tests, its rows in `kavach/Ops.h`, its
declarations in `types/forge.d.ts` and its WinterTC.md rows (decision D20:
an entry is ✅ only when tests cover it).

| Step | APIs | Notes |
|---|---|---|
| 10.1 | `DOMException`, `atob`, `btoa`, `TextEncoder`, `TextDecoder` | The architecture of section 2. Done: see HISTORY.md. |
| 10.2 | `EventTarget`, `Event`, `CustomEvent`, `ErrorEvent`, `PromiseRejectionEvent`, `reportError`, the global `error` / `unhandledrejection` / `rejectionhandled` events and `on…` properties, `self`, `navigator.userAgent` (`"Forge"`) | Done: section 5. An uncaught error first fires `error` on the program's global; exit code 1 only if no listener cancels it (D28). |
| 10.3 | `AbortController`, `AbortSignal` (`abort`, `timeout`, `any`), the `signal` option of `addEventListener`; WPT coverage (section 4) | Section 6. WPT cases are transcribed and recorded, not run (D29, section 4). |
| 10.4 | `structuredClone`, `MessageChannel`, `MessagePort`, `MessageEvent` | Clone through the engine's structured clone with Forge's callbacks; transfer of `ArrayBuffer` and `MessagePort`. |
| 10.5 | `URL`, `URLSearchParams`, `URLPattern` | A WHATWG URL parser in portable C++ (unit-tested and fuzzed like `runtime/Encoding`), with the WPT `urltestdata.json` cases. |
| 10.6 | `performance` (`now`, `timeOrigin`, marks and measures), `crypto` (`getRandomValues`, `randomUUID`, `subtle`) | `performance.now()` is coarsened (Spectre). Randomness and `SubtleCrypto` on Windows CNG (BCrypt), never a JS implementation. |
| 10.7 | `Blob`, `File`, `FormData` | |
| 10.8 | Streams (13 interfaces), `TextEncoderStream`, `TextDecoderStream`, `CompressionStream`, `DecompressionStream` | Compression on a vendored zlib-compatible library or Windows' own; chosen when 10.8 starts. |

## 2. Architecture (decision D27)

**D27. A web API is a built-in JavaScript file for its WebIDL surface over
C++ natives for its byte work; its globals are intrinsics.**

- **WebIDL in JavaScript.** Argument conversion, brand checks, prototypes,
  getters, constants and internal state are written in JavaScript
  (`runtime/WebApis.js`), which follows the WebIDL rules directly: classes
  with private fields, members made enumerable, `@@toStringTag`, global
  properties writable, configurable and (for interfaces) not enumerable.
  Writing this in JSAPI C++ would be several times longer and easier to
  get subtly wrong; the cost is that the script runs in each realm.
- **Byte work in C++.** Anything that loops over bytes or code units is
  portable C++ with no JSAPI (`runtime/Encoding.cpp` in 10.1), tested in
  both CI test jobs and fuzzed (rule R24), and reached through a
  **natives** object. The natives are passed to the script as an argument;
  they are never on a global, so no other script can reach them, and they
  trust nothing they are given (a wrong argument is a TypeError, never a
  crash).
- **Primordials.** `runtime/WebApis.cpp` runs the script in each realm
  right after `JS::InitRealmStandardClasses`, before any other script. The
  script captures every built-in it needs when called (`WeakMap.prototype.get`
  through `Function.prototype.call.bind`, `Object.defineProperty`, ...)
  and never uses the array iterator, so user code that replaces a built-in
  can neither break a web API nor observe its internals. WebIDL
  conversions (`ToString` of an argument, getters of an options object)
  do run user code, where the standards say so and nowhere else
  (CallSites.md).
- **Intrinsics.** The globals the script defines are walk roots for the
  lockdown (Phase 9.3) next to the engine's: in a package compartment they
  are frozen like `Array`, and one that crosses compartments is replaced by
  the receiving side's own, so `instanceof DOMException` works across
  compartments and no package can hold the program's `TextDecoder.prototype`.
  The op-table self-test (R2) still treats them as Forge's: each global has
  a `kavach/Ops.h` row (`Permission::None`).
- **Crossing compartments.** An instance crosses as an ordinary wrapper.
  Getters work through it (they run in the instance's compartment). A
  method called on another compartment's instance fails its brand check in
  the caller's compartment; the script then asks the `forward` native,
  which calls the instance's own compartment's member on the instance,
  with the arguments and the result crossing through the wrapper policy.
  This is what the engine does for its own built-ins (`Map.prototype.get`
  on a wrapper). It gives the caller exactly what holding the object
  already gives: object-capability semantics.
- **Errors.** Errors a web API throws are made by the `makeError` native,
  whose stack, file and line start at the first frame outside Forge's own
  scripts (source names starting with `forge:`), so an uncaught
  `DOMException` points at the user's line. A `DOMException` is a real
  `Error` object whose prototype is `DOMException.prototype`; its name,
  message and code live in a WeakMap, which console output reads from C++
  without running a getter (`GetDomExceptionFields`).
- **Kavach.** No 10.x API needs a permission. For the network APIs of
  Phase 11 the rule is: the Kavach decision is made by the function that
  creates the capability (`fetch()`, a `WebSocket` constructor), with the
  policy of the realm it runs in (D24), never by a method of the object it
  returns. Forwarding therefore cannot move a decision into another
  compartment.

Alternatives considered: JSAPI C++ for everything (more code, more places
to get WebIDL wrong, no benefit for security since the C++ would make the
same calls); SpiderMonkey's self-hosted code (not available to embedders);
taking a polyfill (`core-js`, Deno's or Node's JavaScript) (written
against other runtimes' internals, and not built to keep working after
user code changes the built-ins).

## 3. Phase 10.1

**Encodings (`runtime/Encoding.cpp`).** UTF-8 encode (lone surrogates
become U+FFFD) and `encodeInto`; the WHATWG UTF-8 decoder (each maximal
invalid subpart becomes one U+FFFD); the shared UTF-16 decoder (BE and LE);
the 28 single-byte encodings and x-user-defined, with tables generated by
`tools/gen-encoding-tables.py` from the standard's own index files (the
generated file records each index's identifier); WHATWG label lookup for
all 228 labels; forgiving-base64 decode and base64 encode. Streaming is
stateless: a decoder reports how many bytes it consumed, and `TextDecoder`
keeps the rest for the next call.

**Documented deviations (WinterTC.md):**

- The CJK multi-byte encodings (GBK, gb18030, Big5, EUC-JP, ISO-2022-JP,
  Shift_JIS, EUC-KR) are not decoded yet: their labels throw a
  `RangeError` that says so. They need large index tables and will come
  with a size budget.
- After a fatal decoding error, a `TextDecoder` starts a new stream (the
  standard leaves the rest of that stream's input queued).
- `atob` and `btoa` do not check their `this` value, like Node and Deno.

**Proof.** `tests/web-apis.js` (the WPT cases for these APIs, transcribed;
see section 4), `tests/web-uncaught.js`, the `webby` package of the
isolation test (crossing compartments both ways), `runtime/EncodingTest.cpp`,
`runtime/fuzz/EncodingFuzz.cpp`, and the web APIs in `tests/fuzz-natives.js`.
During development the encoders were also compared against Node's
TextDecoder and atob on random inputs, and every table difference was
checked against the WHATWG index (each was ICU departing from it).

## 4. Web Platform Tests (decision D29)

**D29. Forge checks its web APIs against WPT without running WPT code.**
Each WPT case is transcribed by hand into Forge's own tests and reviewed
like the rest of Forge's code; `tests/wpt/coverage.json` records, for one
pinned WPT commit, every subtest of every tracked WPT file and the Forge
check that covers it, or why it does not apply.

The first plan was to vendor a pinned subset of WPT (its test files and
`testharness.js`) and run it in CI through a small runner. It was dropped
in 10.3 because it would run third-party code, from a repository with
thousands of contributors, in CI and on the build machine. A harness
written by Forge would not change that: the test files are code too.
Transcription costs more effort per API, but what it gives up is small:

- **Exactness is kept.** `tools/wpt-coverage.py --check` (CI, offline)
  fails if the record names a Forge check that does not exist.
  `--upstream <clone>` reads a WPT clone as text (never running it) and
  lists every subtest not yet recorded, every recorded file that changed
  since the pin, and every file of a tracked directory that the record does
  not list; a maintainer runs it when moving the pin or adding an API.
- **WinterTC.md cites the record** (subtests covered, files that do not
  apply and why) instead of a runner's pass count.
- **Licensing.** Transcribed cases derive from WPT and stay under its
  3-Clause BSD License (`tests/wpt/LICENSE.md`).

The cases transcribed in 10.1 and 10.2 predate the record and are to be
added to it.

## 5. Phase 10.2

**The global object is an EventTarget**, as a worker's global is: its
prototype is `EventTarget.prototype` (a SpiderMonkey global's prototype is
not immutable), its listeners live in the same WeakMap as every other
target's, and, as WebIDL says, an operation called with an undefined `this`
acts on the global object, so a bare `addEventListener("error", f)` works.
`self` (a [Replaceable] accessor), `navigator` (a frozen object whose
`userAgent` is `"Forge"`; `Navigator` itself is not a global) and the three
event handler attributes are own accessors of the global object. The
handlers follow the HTML rules: a handler's listener is added when it is
first set to an object and keeps its place when replaced; `onerror` gets
`(message, source, lineno, colno, error)` and cancels by returning true;
the others cancel by returning false.

**Dispatch** follows the DOM Standard for a target that is not a node: the
path is the target alone, so capturing listeners run first, then the
others, all at `AT_TARGET`; `once`, `passive`, `stopImmediatePropagation`,
listeners added or removed during dispatch, `handleEvent` looked up at each
dispatch, and `isTrusted` (an own, unforgeable accessor) all as specified.
An exception thrown by a listener is reported (below) and the dispatch goes
on.

**Uncaught errors and rejections (decision D28).**

**D28. Uncaught errors and unhandled rejections are events at the
program's global object only.** Every place where Forge used to end the
program for an uncaught exception (the main module, a timer, a microtask, a
signal listener, a top-level `await`), an event listener's exception and
`reportError()` now run the HTML "report an exception" steps: an
`ErrorEvent` (cancellable, trusted, its fields taken from the error report
without running script) is dispatched at the program's global object. Only
if no listener cancels it is the error printed and the exit code 1 (the
output is unchanged). Unhandled rejections likewise fire
`unhandledrejection`, and a rejection whose event was cancelled fires
`rejectionhandled` when it later gets a handler. An exception thrown by an
error listener itself is uncaught at once, with no second event. These
events are never dispatched at a package's global, even for a package's
own errors: packages in the shared pure compartment share one global, and
none of them may hide another's errors, so the error policy belongs to the
program. Why not per realm, as HTML would for separate windows: in Forge
the program is responsible for the whole process, and a package that
silently swallowed failures would be a supply-chain foothold.

When an error is not cancelled, the rest of the current task still runs
(the code after `dispatchEvent()` or `reportError()`), then the program
ends; Deno behaves the same way. A cancelled error in the main module's
top-level code lets the program go on with its timers and promises.

**Documented deviations (WinterTC.md):**

- An `Event` can be dispatched only at an `EventTarget` of its own
  compartment: the program cannot dispatch its own `Event` at a package's
  target (a `TypeError`). Listeners and dispatches across compartments
  otherwise work (a listener may belong to any compartment).
- `addEventListener`'s `signal` option arrived with `AbortSignal` (10.3,
  section 6).
- `Event.timeStamp` counts milliseconds since Forge started, in steps of
  0.1 ms (Spectre), the step browsers use without cross-origin isolation.

**Proof.** `tests/web-events.js` (36 groups: the WPT cases for these APIs,
transcribed, plus the global object, handlers, `reportError` and patched
built-ins), `tests/web-error-handled.js` (cancelled errors from a listener,
`reportError`, a top-level throw, a microtask and a timer; a cancelled
rejection handled later), `tests/web-error-unhandled.js` (a listener that
does not cancel; an exception inside an error listener), the `webby`
package (a program listener on a package's target; a package's rejection
seen only by the program), and the event APIs in `tests/fuzz-natives.js`.

## 6. Phase 10.3

**AbortController and AbortSignal** follow the DOM Standard: `abort()`
(the default reason is an `"AbortError"` DOMException), the static
`abort()`, `timeout()` and `any()`, `aborted`, `reason`,
`throwIfAborted()` and the `onabort` event handler. Aborting runs the
signal's abort algorithms and then fires a trusted `abort` event, and a
signal made by `AbortSignal.any()` (a dependent signal) is marked aborted
before any abort event fires, as the standard's "signal abort" steps say.
`AbortSignal.any()` of a dependent signal follows that signal's sources,
never the dependent signal itself, so chains stay one level deep.

`AbortSignal` is not written as `class ... extends EventTarget`: `super()`
would call whatever the class's prototype is at that moment, which user
code can change, and `timeout()` and `any()` must not run user code there.
The prototype chain is set by hand instead, and only `WebApis.js` can make
a signal (`new AbortSignal()` is a `TypeError`, as the standard says). The
same concern applied to 10.2: the `ErrorEvent` and `PromiseRejectionEvent`
Forge makes for an uncaught error or rejection reach `Event` through
`super()`. They are now made with the original parent in place
(`makeInternalEvent`), so reporting an error never runs user code there.

**The `signal` option of `addEventListener`.** An aborted signal adds
nothing; otherwise aborting it later removes the listener (one abort
algorithm per listener). Algorithms whose listener is already gone (after
`removeEventListener` or `once`) are dropped from time to time, so a
long-lived signal used for many short-lived listeners does not grow
without bound. `null` is a `TypeError`, as WebIDL says, since the member
is not nullable.

**Event handler attributes, for any target.** The handler rules of 10.2
(the listener is added when the handler is first set to an object and
keeps its place when replaced) now live in one place, keyed by target and
event type: the global object's `onerror`, `onunhandledrejection` and
`onrejectionhandled` and `AbortSignal`'s `onabort` all use it.

**`AbortSignal.timeout()` and internal timers.** The timeout is one of
Forge's timers in the signal's realm, flagged *internal* (forge.cpp,
`HostScheduleInternalTimer`): it does not keep the program running, as in
Node and Deno, and `clearTimeout()` cannot cancel it, so no code in the
same realm can stop a timeout by guessing its timer id. The event loop
stops when every pending timer is internal. A program that wants to wait
for a timeout keeps itself running with an ordinary timer or a promise it
is awaiting.

**Timer order.** Timeouts that are due together fire in the order they
were made, as WPT's `timeout.any.js` checks. Writing that test found that
forge-core's scheduler fired due timers in vector order, which removals
scrambled; `TimerScheduler::PopDue` now always fires the earliest due
timer next, ties broken by scheduling order (this also fixes `setTimeout`).

**Garbage collection of dependent signals.** The DOM Standard says a
source holds its dependent signals weakly, and a dependent signal that has
`abort` listeners or abort algorithms must stay alive while a source that
can abort it does. Forge links each dependent to each source with a small
record that both hold: the record holds the dependent through a `WeakRef`,
and also strongly while the dependent has listeners or algorithms. The
dependent holds a source of its own compartment through a `WeakRef`. So a
dependent that nobody can observe is collected even if its source lives on
(the usual pattern: a long-lived controller combined with a per-request
timeout), and one that has a listener lives exactly as long as a source
that can fire it. Links to dependents that are gone or aborted are dropped
from time to time.

**Signals across compartments.** A signal of another compartment is
accepted wherever a signal is (the `signal` option, `AbortSignal.any()`).
Its own compartment answers through hidden members of the `forward`
mechanism (`AbortSignal.~status`, `~reason`, `~follow`): they are in the
members table only, on no prototype, and never throw. Following a foreign
signal registers an abort algorithm of this compartment's in the signal's
own compartment; an exception from it is reported, not thrown into the
other compartment's abort. A dependent holds a foreign source strongly (a
`WeakRef` to a cross-compartment wrapper would not say whether the signal
itself is alive).

**Documented deviations (WinterTC.md):**

- A dependent signal that follows a signal of *another* compartment is
  aborted (algorithms and its `abort` event) while that source runs its
  abort algorithms, so before the source's own `abort` event; with a source
  of its own compartment the order is exactly the standard's.
- A program whose only pending work is an `AbortSignal.timeout()` ends
  without the timeout firing (as in Node and Deno). Browsers have no such
  notion.

**Proof.** `tests/web-abort.js` (59 checks: every subtest of WPT's
`dom/abort/` that a runtime without frames can run, and every case of
`AddEventListenerOptions-signal.any.js`, transcribed and recorded in
`tests/wpt/coverage.json` (46 subtests; 4 files need iframes or a browser's
garbage-collection hook); plus brand checks,
the members and their order, argument conversion, the handler rules,
reported listener errors, `clearTimeout` not cancelling a timeout, and
patched built-ins), `tests/web-abort-unref.js` (a pending timeout does not
keep the program running; the runner checks the run ends within 20 s), the
`webby` package (a program signal removing a package's listener and
followed by a package's `AbortSignal.any()`, and the reverse), and the
abort APIs in `tests/fuzz-natives.js`.
