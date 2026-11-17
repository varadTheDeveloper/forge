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
| 10.2 | `EventTarget`, `Event`, `CustomEvent`, `ErrorEvent`, `PromiseRejectionEvent`, `reportError`, the global `error` / `unhandledrejection` / `rejectionhandled` events and `on…` properties, `self`, `navigator.userAgent` (`"Forge"`) | An uncaught error first fires `error` on the global; exit code 1 only if no listener prevents it. The WPT runner (section 4) lands here, since `testharness.js` needs `self` and events. |
| 10.3 | `AbortController`, `AbortSignal` (`abort`, `timeout`, `any`) | `AbortSignal.timeout` uses Forge's timers, per realm. |
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

## 4. Web Platform Tests

Until 10.2, WPT cases are transcribed into Forge's own tests. From 10.2, a
pinned subset of WPT (the test files and `testharness.js`, vendored under
`third_party/wpt/` with its commit) runs in CI through a small runner that
reports each file's results; WinterTC.md then cites the pass counts. A
known failure is listed in an expectations file with its reason, so a
regression and a fix both show up as a change to that file.
