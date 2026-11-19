# WinterTC Minimum Common API: Forge's status

The [Minimum Common Web API](https://min-common-api.proposal.wintertc.org/)
is the list of web APIs that server-side JavaScript runtimes agree to share,
so the same code runs on all of them. It is published by Ecma TC55
(WinterTC). Forge has adopted it as its target for web APIs (decision D13);
most entries arrive in Phases 10 and 11.

**Forge does not conform yet.** This table shows exactly where it stands. It
is updated whenever an entry changes. How the web APIs are built, and the
order of the rest of Phase 10: [PHASE10_DESIGN.md](PHASE10_DESIGN.md).

| Status | Meaning |
|---|---|
| ✅ Yes | Implemented in Forge and covered by Forge's tests |
| 🟡 Partial | Implemented with a documented gap |
| ⚙️ Engine | Provided by SpiderMonkey; not yet covered by Forge's tests |
| ❌ No | Not implemented yet; the phase that adds it is given |

**Summary (2026-11-17):** of the 25 global functions and properties, 14
are ✅, 1 is 🟡, 4 are ⚙️ and 6 are ❌. Of the 53 interfaces, 9 are ✅, 1
is 🟡, 10 are ⚙️ (all WebAssembly) and 33 are ❌. (The Phase 10.1 summary
counted `atob()` and `btoa()` as one entry; they are two.)

## Global functions and properties (§5.2)

| Entry | Status | Notes |
|---|---|---|
| `globalThis` | ✅ Yes | |
| `setTimeout()` | ✅ Yes | |
| `setInterval()` | ✅ Yes | |
| `clearTimeout()` | ✅ Yes | |
| `clearInterval()` | ✅ Yes | Added in Phase 8.2 |
| `queueMicrotask()` | ✅ Yes | |
| `console` | 🟡 Partial | `console.table` prints like `console.log` |
| `WebAssembly.compile()` | ⚙️ Engine | Can be turned off with `--disallow-wasm` |
| `WebAssembly.instantiate()` | ⚙️ Engine | |
| `WebAssembly.validate()` | ⚙️ Engine | |
| `WebAssembly.JSTag` | ⚙️ Engine | |
| `WebAssembly.compileStreaming()` | ❌ No | Needs `Response`; Phase 11 |
| `WebAssembly.instantiateStreaming()` | ❌ No | Needs `Response`; Phase 11 |
| `atob()`, `btoa()` | ✅ Yes | Phase 10.1 (forgiving-base64, WebIDL conversions, `InvalidCharacterError`) |
| `structuredClone()` | ❌ No | Phase 10 |
| `reportError()` | ✅ Yes | Phase 10.2 |
| `self` | ✅ Yes | Phase 10.2 |
| `navigator.userAgent` | ✅ Yes | Phase 10.2: `"Forge"` |
| `onerror`, `onunhandledrejection`, `onrejectionhandled` | ✅ Yes | Phase 10.2, with the `error`, `unhandledrejection` and `rejectionhandled` events on the global object (an `EventTarget`). An uncaught error or unhandled rejection ends the program with exit code 1 only if no listener cancels its event. |
| `performance` | ❌ No | Phase 10; the timer will be coarsened (Spectre) |
| `crypto` | ❌ No | Phase 10, on Windows CNG |
| `fetch()` | ❌ No | Phase 11 |

The three `on…` properties, and `atob()` and `btoa()`, are listed
separately in the specification; the table groups them, so its 22 rows
count 25 entries.

## Interfaces (§5.1)

| Group | Interfaces | Status |
|---|---|---|
| WebAssembly | `Global`, `Instance`, `Memory`, `Module`, `Table`, `Tag`, `Exception`, `CompileError`, `LinkError`, `RuntimeError` | ⚙️ Engine |
| DOM | `Event`, `EventTarget` | ✅ Yes (Phase 10.2) |
| DOM | `AbortController`, `AbortSignal` | ✅ Yes (Phase 10.3): `abort`, `timeout`, `any`, `throwIfAborted`, `onabort`, and the `signal` option of `addEventListener`. WPT: all 46 subtests of `dom/abort/` and `AddEventListenerOptions-signal.any.js` that need no browser feature are covered ([tests/wpt/coverage.json](tests/wpt/coverage.json)) |
| HTML | `CustomEvent`, `ErrorEvent`, `PromiseRejectionEvent` | ✅ Yes (Phase 10.2) |
| HTML | `MessageChannel`, `MessageEvent`, `MessagePort` | ❌ No (Phase 10.4) |
| WebIDL | `DOMException` | ✅ Yes (Phase 10.1) |
| Encoding | `TextEncoder` | ✅ Yes (Phase 10.1) |
| Encoding | `TextDecoder` | 🟡 Partial (Phase 10.1): UTF-8, UTF-16LE/BE, all 28 single-byte encodings and x-user-defined; the CJK multi-byte encodings throw a `RangeError` for now |
| Encoding | `TextDecoderStream`, `TextEncoderStream` | ❌ No (Phase 10.8, with Streams) |
| URL | `URL`, `URLSearchParams`, `URLPattern` | ❌ No (Phase 10) |
| File API | `Blob`, `File` | ❌ No (Phase 10) |
| XHR | `FormData` | ❌ No (Phase 10) |
| Streams | `ReadableStream` and its readers, controllers and BYOB request, `WritableStream` and its writer and controller, `TransformStream` and its controller, `ByteLengthQueuingStrategy`, `CountQueuingStrategy` (13 in all) | ❌ No (Phase 10) |
| Compression | `CompressionStream`, `DecompressionStream` | ❌ No (Phase 10) |
| Web Crypto | `Crypto`, `CryptoKey`, `SubtleCrypto` | ❌ No (Phase 10) |
| High Resolution Time | `Performance` | ❌ No (Phase 10) |
| Fetch | `Headers`, `Request`, `Response` | ❌ No (Phase 11) |

## Planned documented deviations

The specification allows a runtime to deviate from a requirement for
technical or structural reasons, if the deviation is documented. Forge's:

- **Permissions.** An operation Kavach refuses (for example, `fetch` to a
  host that is not granted) fails with a `PermissionDenied` error. See
  [Kavach.md](Kavach.md).
- **No origin.** Like every server runtime, Forge has no web origin, so
  `fetch` cannot send an `Origin` header the way a browser does. The
  specification gives this as its own example.
- **Coarse timers.** `performance.now()` will be coarsened by default, as
  browsers do, to make timing attacks harder.

## Documented deviations in what is implemented

- **TextDecoder encodings (Phase 10.1).** GBK, gb18030, Big5, EUC-JP,
  ISO-2022-JP, Shift_JIS and EUC-KR are not decoded yet; their labels throw
  a `RangeError` saying so. Every other encoding and label of the Encoding
  Standard is supported.
- **TextDecoder after a fatal error (Phase 10.1).** The decoder starts a
  new stream; the standard leaves the rest of the failed stream queued.
- **`atob` and `btoa` (Phase 10.1)** do not check their `this` value, as in
  Node and Deno.
- **Packages (Phase 10.1).** Inside a package the web API classes are
  frozen, like every other built-in there (PHASE9_DESIGN.md section 5).
- **Uncaught errors in packages (Phase 10.2).** The `error`,
  `unhandledrejection` and `rejectionhandled` events are dispatched only at
  the program's global object, also for a package's own errors; a
  package's global never receives them (PHASE10_DESIGN.md, D28).
- **Events across compartments (Phase 10.2).** An `Event` can be
  dispatched only at an `EventTarget` of its own compartment.
- **`AbortSignal.timeout()` (Phase 10.3)** does not keep the program
  running, as in Node and Deno: a program whose only pending work is a
  timeout ends without it firing. `clearTimeout()` cannot cancel it.
- **Signals across compartments (Phase 10.3).** A dependent signal
  (`AbortSignal.any()`) that follows a signal of another compartment is
  aborted before that source's own `abort` event fires; with a source of
  its own compartment the order is the standard's (PHASE10_DESIGN.md
  section 6).
- **`Event.timeStamp` (Phase 10.2)** has a resolution of 0.1 ms.

## Forge's own APIs

The specification allows extra APIs but asks runtimes to keep them few and
apart from the standard ones. Forge's extras live under one frozen `Forge`
namespace (`Forge.args`, `Forge.env`, `Forge.exit`, ...). The `fs` global
moves to `Forge.fs` in Phase 9 (decision D14).
