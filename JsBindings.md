# JS Bindings — Conventions

## Purpose

Defines the conventions every JS-visible binding in `forge.cpp` follows
from Phase 7 onward: how a `forge-core` `Result<T>`/`Result<void>` becomes
either a JS return value or a thrown JS `Error`, how bytes and strings
cross the JS/native boundary, and how new JS-visible surface is named.
`Fs.md` is the first concrete surface built on this doc, the same way
`File.md`/`Socket.md` are each built on the lower-level `Path.md`/
`IpAddress.md`. Future networking (`net`) and threading (`threads`)
bindings are expected to reuse this doc's helpers unchanged rather than
each inventing their own error/marshalling convention — see
"Extensibility" below.

## Responsibilities

- Define the shape of a JS exception thrown for a failed `Result<T>`/
  `Result<void>`, and the `ErrorCode` → string mapping used to populate
  it.
- Define marshalling helpers for JS string ↔ `forge::core::String`/
  `StringView`/`Path`, and for native bytes ↔ JS `Uint8Array`/
  `ArrayBuffer`.
- Define the naming convention new JS-visible globals follow.
- Define what does and doesn't belong in this doc vs. a concrete surface
  spec like `Fs.md`.

## Non-Goals

- Not a module system design (there isn't one yet; see `ROADMAP.md`'s
  Phase 7 entry for where that sits relative to this work).
- Not an exhaustive `ErrorCode` table for every enumerator that could
  ever exist — only the ones a real binding actually surfaces need an
  entry; extend the table in "Error Handling Policy" as new codes are
  actually reached, rather than speculatively filling it in now.
- Not committing to an async I/O story *in this doc*. Promise-based
  async fs.* has since landed as Phase 7.5 (specified in `FsAsync.md`,
  now real-build-confirmed through 7.5.5) — as a new, additive spec that
  depends on this one without modifying it, per `FsAsync.md`'s own
  Purpose section. This doc's own conventions (Error Handling Policy,
  marshalling helpers, etc.) are unchanged by that work.
- Not a `Buffer`-compatibility layer (Node's `Buffer` is a userspace
  subclass of `Uint8Array` with extra methods) — plain `Uint8Array`/
  `ArrayBuffer` only. A `Buffer`-like convenience type is future work if
  ever needed, not part of this design.

## Design Goals

- Every future binding (fs now; net/threads later) composes the *same*
  helpers from this doc rather than writing its own error-to-exception or
  string/byte marshalling code inline in `forge.cpp` — one place to get
  this right, one place to fix it if it's wrong.
- Errors must be genuinely catchable and inspectable from JS (a `.code`
  a script can branch on), not just a opaque thrown string — mirrors how
  `Error` in `Error.h` already carries a structured `ErrorCode` rather
  than a message.
- No behavior invented beyond what `forge-core` already implements. If a
  concrete surface spec (`Fs.md` and beyond) wants something `forge-core`
  doesn't yet support, that's a Non-Goal in that spec and a candidate
  for new `forge-core` work, not something the binding fakes.

## Public API (helpers `forge.cpp` gets from this design)

None of these are JS-visible themselves — they're the internal C++
helpers every JS-visible binding function calls into.

```cpp
// Error mapping. Throws a JS Error via JS_ReportErrorASCII/an equivalent
// that also sets an own `code` property, populated from `error.Code()`
// via the ErrorCode table below. `context` names the calling JS function
// (e.g. "readFileSync") and becomes the thrown Error's `syscall`
// property; `path`, if given, becomes the thrown Error's `path` property
// (both are additive metadata, not required by any engine machinery).
// Returns nothing meaningful — the caller's JS-visible function returns
// `false` right after calling this, same convention `JS_ReportOutOfMemory`
// already uses in Phase 6's SetTimeout/SetInterval/EnqueueMicrotask.
void ThrowJsError(JSContext* cx, const forge::core::Error& error,
                   const char* context, const forge::core::Path* path = nullptr);

// String/Path marshalling.
forge::core::Result<forge::core::String> ToForgeString(JSContext* cx, JS::HandleValue value);
forge::core::Result<forge::core::Path>   ToForgePath(JSContext* cx, JS::HandleValue value);
JSString* FromForgeString(JSContext* cx, forge::core::StringView text); // nullptr on OOM; caller reports

// Binary marshalling.
JSObject* Uint8ArrayFromBytes(JSContext* cx, forge::core::Vector<forge::core::u8> bytes); // consumes `bytes`
// Reads a JS Uint8Array/ArrayBuffer's bytes without copying into a
// forge-core container — returns a Span<const u8> aliasing the JS
// buffer's own storage, valid for the duration of the call site.
forge::core::Result<forge::core::Span<const forge::core::u8>> AsByteSpan(JSContext* cx, JS::HandleValue value);
```

## Error Handling Policy

Every JS-visible binding function follows the same shape: call into
`forge-core`, and on `Result<T>::HasError()`/`Result<void>::HasError()`,
call `ThrowJsError(cx, result.Error(), "<function name>", pathOrNull)`
then `return false;` — never a bare `JS_ReportErrorASCII` with a hand-
written message, so every failure is uniformly catchable by `.code`
rather than by string-matching a message (string-matching error messages
is exactly the kind of brittle pattern this convention exists to avoid).

**Design clarification, added during Phase 7.2 implementation (behavioral
clarification, not a change to any signature above — recorded here before
implementing per this phase's own "document deviations before
implementing them" rule):** `ToForgeString`/`ToForgePath` (and any future
sibling marshalling helper returning `Result<T>`) never leave their own
exception pending on `cx` when they return a failed `Result<T>`. If an
underlying `JS::`/`JS_*` call they compose (`JS::ToString`,
`JS_EncodeStringToUTF8`, …) already reported an exception before they
detect the failure, they call `JS_ClearPendingException(cx)` before
constructing the `Result<T>` failure. This means every caller can
uniformly write:

```cpp
Result<T> converted = ToForgeString(cx, value); // or any Result<T>-returning helper here
if (converted.HasError()) {
  ThrowJsError(cx, converted.Error(), "<function name>");
  return false;
}
```

without ever risking two exceptions pending on `cx` at once (one left
over from the underlying `JS::`/`JS_*` call, one freshly set by
`ThrowJsError`). Binding authors should not call `JS_IsExceptionPending`
before `ThrowJsError` to guard against this — it can't happen, by this
contract.

`ErrorCode` → `error.code` string table (extend as new codes are reached
by a real binding; do not populate speculatively):

| `ErrorCode` | `error.code` |
|---|---|
| `NotFound` | `"NotFound"` |
| `AlreadyExists` | `"AlreadyExists"` |
| `PermissionDenied` | `"PermissionDenied"` |
| `InvalidArgument` | `"InvalidArgument"` |
| `InvalidOperation` | `"InvalidOperation"` |
| `NotSupported` | `"NotSupported"` |
| `NotImplemented` | `"NotImplemented"` |
| `Busy` | `"Busy"` |
| `Timeout` | `"Timeout"` |
| `Cancelled` | `"Cancelled"` |
| `IOError` | `"IOError"` |
| `BufferTooSmall` | `"BufferTooSmall"` |
| `Overflow` | `"Overflow"` |
| `Underflow` | `"Underflow"` |
| `InvalidData` | `"InvalidData"` |
| `ParseError` | `"ParseError"` |
| `PlatformError` | `"PlatformError"` (also sets `.nativeCode` from `Error::NativeCode()` — per `Error.h`'s own doc comment, this is the "inspect `NativeCode()`" case) |
| `Unknown` | `"Unknown"` |
| `OutOfMemory` | **not routed through `ThrowJsError`** — call `JS_ReportOutOfMemory(cx)` directly, matching the existing Phase 6 convention in `EnqueueMicrotask`/`SetTimeout`/`SetInterval`. Consistency with already-shipped code outweighs uniformity here. |
| `None` | never surfaces (means "no error") — hitting this in `ThrowJsError` is a programming error in the caller, not a real failure to report. |
| `EndOfFile` | not an error at all in `forge-core`'s own convention (`File::Read`/`Socket::Receive` return `0` on EOF, not an error) — should never reach `ThrowJsError`; listed for completeness only. |

## Thread Safety

N/A at this layer — every helper above runs on the single JS/event-loop
thread, same as every other JS-visible function in `forge.cpp` today.
(Async variants built on `ThreadPool` later will need their own thread-
safety note in whatever spec introduces them — not this one, since
nothing here crosses threads.)

## Dependencies

`Error.h`, `Result.h`/`ResultVoid.h`, `String.h`, `StringView.h`, `Path.h`,
`Span.h`, `memory/Vector.h` — all already real-build-confirmed or pure
logic. No new `forge-core` component required for this doc itself.

## Naming Conventions (for all future JS-visible surface)

- New JS-visible capability groups live under a namespaced global object
  (`globalThis.fs`, later `globalThis.net`, `globalThis.threads`), not as
  flat global functions — the runtime already has five flat globals
  (`print`, `setTimeout`, `setInterval`, `clearTimeout`,
  `queueMicrotask`); adding fs/net/threads as more flat globals would
  make collisions and discoverability worse just as the surface starts
  growing for real. `print`/timers/`queueMicrotask` stay flat since
  they're already shipped and changing them isn't this doc's concern.
- Method names mirror Node's naming wherever a direct analog exists
  (`readFileSync`, `writeFileSync`, `existsSync`, `mkdirSync`, `rmSync`)
  specifically so existing JS knowledge transfers — this is a *naming*
  convenience, not a compatibility promise. Divergence from Node's exact
  behavior is expected and fine wherever `forge-core`'s own semantics
  differ (see each concrete surface spec's own Non-Goals for where that
  applies).
- Every namespace's synchronous methods use the `*Sync` suffix from the
  start, even before any async counterpart exists — reserves the
  unsuffixed name (`fs.readFile`) for the Promise-returning version. This
  played out exactly as intended for `fs`: Phase 7.5 (`FsAsync.md`) added
  `fs.readFile`/etc. using the reserved bare names, with no rename of
  `fs.readFileSync`/etc. needed.

## Extensibility

Networking (`net`) and threading (`threads`) bindings, whenever they're
scoped, should reuse `ThrowJsError`/`ToForgeString`/`ToForgePath`/
`Uint8ArrayFromBytes`/`AsByteSpan` unchanged — `Socket`'s and
`Thread`/`ThreadPool`'s public APIs already return the same `Result<T>`/
`Result<void>` shape `File`/`Path` do, so nothing about this doc's error
or marshalling conventions is fs-specific. The one piece each future
binding will still need to design itself is its own concrete Public API
surface (a `net.md`/`threads.md` following `Fs.md`'s shape) and, for
threading specifically, a message-passing/structured-clone story this
doc deliberately does not attempt to anticipate.

## Acceptance Criteria

This doc is frozen once reviewed and approved. `Fs.md` (and later
`net.md`/`threads.md`) may reference it but should not re-derive or
duplicate its conventions. Implementation of the helpers in "Public API"
above is Phase 7.2 in `ROADMAP.md` — this doc alone has nothing to
compile or verify.

## Implementation Status

Design only. Not yet implemented.
