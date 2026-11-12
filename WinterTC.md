# WinterTC Minimum Common API: Forge's status

The [Minimum Common Web API](https://min-common-api.proposal.wintertc.org/)
is the list of web APIs that server-side JavaScript runtimes agree to share,
so the same code runs on all of them. It is published by Ecma TC55
(WinterTC). Forge has adopted it as its target for web APIs (decision D13);
most entries arrive in Phases 10 and 11.

**Forge does not conform yet.** This table shows exactly where it stands. It
is updated whenever an entry changes.

| Status | Meaning |
|---|---|
| ✅ Yes | Implemented in Forge and covered by Forge's tests |
| 🟡 Partial | Implemented with a documented gap |
| ⚙️ Engine | Provided by SpiderMonkey; not yet covered by Forge's tests |
| ❌ No | Not implemented yet; the phase that adds it is given |

**Summary (2026-11-12):** of the 25 global functions and properties, 6 are
✅, 1 is 🟡, 4 are ⚙️ and 14 are ❌. Of the 53 interfaces, 10 are ⚙️ (all
WebAssembly) and 43 are ❌.

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
| `atob()`, `btoa()` | ❌ No | Phase 10 |
| `structuredClone()` | ❌ No | Phase 10 |
| `reportError()` | ❌ No | Phase 10 |
| `self` | ❌ No | Phase 10 |
| `navigator.userAgent` | ❌ No | Phase 10; will be `"Forge"` |
| `onerror`, `onunhandledrejection`, `onrejectionhandled` | ❌ No | Phase 10, with `error` / `unhandledrejection` events on the global object. Today an uncaught error or unhandled rejection ends the program with exit code 1. |
| `performance` | ❌ No | Phase 10; the timer will be coarsened (Spectre) |
| `crypto` | ❌ No | Phase 10, on Windows CNG |
| `fetch()` | ❌ No | Phase 11 |

The three `on…` properties are listed separately in the specification; the
table groups them, so it counts 25 entries.

## Interfaces (§5.1)

| Group | Interfaces | Status |
|---|---|---|
| WebAssembly | `Global`, `Instance`, `Memory`, `Module`, `Table`, `Tag`, `Exception`, `CompileError`, `LinkError`, `RuntimeError` | ⚙️ Engine |
| DOM | `AbortController`, `AbortSignal`, `Event`, `EventTarget` | ❌ No (Phase 10) |
| HTML | `CustomEvent`, `ErrorEvent`, `MessageChannel`, `MessageEvent`, `MessagePort`, `PromiseRejectionEvent` | ❌ No (Phase 10) |
| WebIDL | `DOMException` | ❌ No (Phase 10) |
| Encoding | `TextDecoder`, `TextDecoderStream`, `TextEncoder`, `TextEncoderStream` | ❌ No (Phase 10) |
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

## Forge's own APIs

The specification allows extra APIs but asks runtimes to keep them few and
apart from the standard ones. Forge's extras live under one frozen `Forge`
namespace (`Forge.args`, `Forge.env`, `Forge.exit`, ...). The `fs` global
moves to `Forge.fs` in Phase 9 (decision D14).
