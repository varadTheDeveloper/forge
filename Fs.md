# Fs — JS Filesystem API (Phase 7)

## Purpose

Specifies `globalThis.fs`, the first JS-visible filesystem surface for
Forge, built entirely on the already real-build-confirmed `File`/`Path`
(`forge-core/File.h`, `Path.h`, Phase 3) and the conventions frozen in
`JsBindings.md`. This is the concrete surface spec `JsBindings.md`'s
"Design Goals" describes — read that doc first; this one does not repeat
its error/marshalling conventions. `File.md`'s own Non-Goals section
already anticipated this exact work ("wiring [async I/O] up couples this
component to the event loop and to whatever the JS-visible `fs` API ends
up looking like, which is `forge.cpp`/Runtime-Integration territory (a
later phase)") — this doc is that later phase, for the synchronous
surface first.

## Responsibilities

- Expose a synchronous, script-callable way to read, write, append,
  check existence of, create directories for, remove, and query the size
  of files, backed one-to-one by existing `File`/`Path` methods.
- Surface every failure as a catchable JS `Error` per `JsBindings.md`.

## Non-Goals

- **Async I/O.** Out of scope for *this* doc — `fs.readFile`/etc.
  (Promise-returning) are specified in `FsAsync.md` (Phase 7.5, now
  real-build-confirmed through 7.5.5; see `ROADMAP.md`/`HISTORY.md`), not
  here. This doc's own `*Sync` methods are unchanged by that work — the
  `*Sync` naming convention from `JsBindings.md` existed from the start
  specifically to reserve the unsuffixed names for it, and no rename was
  needed when it landed.
- **Directory removal at all (recursive or not), glob patterns, a
  `force` flag.** `File::Remove`'s Win32 backend is `DeleteFileW` (per
  `File.md`'s Non-Goals list of implemented Win32 calls), which only
  removes files — it cannot remove a directory, empty or not. `fs.rmSync`
  in this spec is therefore file-removal only; calling it on a directory
  path surfaces whatever `Error` `File::Remove` itself returns for that
  case (documented behavior TBD at implementation time — flag this
  precisely rather than guessing). Directory removal (`RemoveDirectoryW`)
  is new `forge-core` work, not in scope for this phase.
- **A `fs.Stats`-shaped stat object.** `File.h` has no file-vs-directory
  type query and no mtime/mode — only `Exists` (existence only, no type)
  and `SizeInBytes()` (requires an open handle). `fs.statSync` in this
  spec therefore returns only `{ size }`. `isFile()`/`isDirectory()`/
  timestamps are out of scope until `forge-core` actually supports
  querying them — do not invent placeholder values for these.
- **Directory listing (`readdirSync`).** Nothing in `File.h`/`Path.h`
  enumerates a directory's contents today. Out of scope for Phase 7;
  flagged here as an obvious future addition once `forge-core` has the
  underlying capability.
- **Streaming reads/writes.** `File::Read`/`Write` already support this
  at the `forge-core` layer (`Seek`/`Tell` plus repeated `Read`/`Write`
  calls), but no JS-visible streaming primitive (a `ReadableStream`-like
  object) is specified here — whole-file `readFileSync`/`writeFileSync`
  only.
- **Symlinks, permissions/mode bits, file locking.** Not supported by
  `File.h` today; not invented here.

## Design Goals

- Every method below is a thin, direct composition of existing `File`/
  `Path` calls — no new `forge-core` behavior implied or required.
- `readFileSync`'s default return type is a `Uint8Array` (binary-safe,
  matches `File::ReadAllBytes` directly), with an explicit `"utf8"`
  encoding argument to get a string instead (matches `File::ReadAllText`
  directly) — mirrors Node's own default (`Buffer` unless `encoding` is
  given), for naming-convention familiarity per `JsBindings.md`.
- `writeFileSync`/`appendFileSync` accept either a JS string or a
  `Uint8Array`/`ArrayBuffer` as input, so a script never needs to
  manually convert text to bytes for the common case.

## Public API

All under `globalThis.fs`. `path` arguments accept a JS string, converted
via `JsBindings.md`'s `ToForgePath`.

```
fs.readFileSync(path: string, encoding?: "utf8"): Uint8Array | string
  // encoding omitted -> Uint8Array via File::ReadAllBytes.
  // encoding === "utf8" -> string via File::ReadAllText.
  // Any other encoding value throws an Error with code "InvalidArgument"
  // (Path.md/File.md's own Result<T> vocabulary; this specific check
  // happens in the binding before calling into File, since it's a JS-
  // argument-shape error, not a forge-core Result<T> failure).

fs.writeFileSync(path: string, data: string | Uint8Array | ArrayBuffer): void
  // Composes File::Open(path, FileMode::Write) + File::Write(...) +
  // File::Close(). A string `data` is UTF-8-encoded first (existing
  // JS_EncodeStringToUTF8 pattern, already proven by Print()).
  // A short write (File::Write returning fewer bytes than requested)
  // is retried by the binding until the full buffer is written or an
  // error occurs -- forge-core's own File::Write policy is "surface a
  // short write as-is, don't retry" (by design, see File.md), so the
  // retry loop belongs in this binding, not in File itself.

fs.appendFileSync(path: string, data: string | Uint8Array | ArrayBuffer): void
  // Same as writeFileSync but File::Open(path, FileMode::Append).

fs.existsSync(path: string): boolean
  // Direct call to File::Exists. Note: existence only, no type
  // information (see Non-Goals) -- returns true for a directory too.

fs.mkdirSync(path: string, options?: { recursive?: boolean }): void
  // options.recursive falsy/omitted (default false, matching Node) ->
  // File::MakeDirectory (fails if any ancestor is missing).
  // options.recursive === true -> File::CreateDirectories (mkdir -p
  // semantics, already implemented exactly this way).

fs.rmSync(path: string): void
  // Direct call to File::Remove (Win32 DeleteFileW) -- file removal
  // only. See Non-Goals: this cannot remove a directory.

fs.statSync(path: string): { size: number }
  // Opens the file (FileMode::Read), calls SizeInBytes(), closes it.
  // Throws NotFound (via the same File::Open failure path as
  // readFileSync) if `path` doesn't exist. See Non-Goals for why this
  // is the entire shape of the returned object.
```

## Ownership

Every `Uint8Array` returned or accepted is either newly allocated for the
call (a fresh copy, own by the JS engine once returned — see
`JsBindings.md`'s `Uint8ArrayFromBytes`, which consumes and transfers a
`forge::core::Vector<u8>`'s storage) or read without copying via
`AsByteSpan` (valid only for the duration of the call, per
`JsBindings.md`) — no binding function retains a reference to a JS-owned
buffer past its own return.

## Error Handling Policy

Exactly `JsBindings.md`'s convention: every `Result<T>`/`Result<void>`
failure throws via `ThrowJsError(cx, error, "<methodName>", &path)` — the
`path` argument is always available here (every method above takes one),
so every thrown `Error` from this module carries `.path` in addition to
`.code`/`.syscall`.

## Thread Safety

N/A — single-threaded event loop, same as every other synchronous
JS-visible function in `forge.cpp`. The Promise-returning counterparts
this note used to forward-reference now exist and do run their actual
I/O on a `ThreadPool` worker thread — see `FsAsync.md`'s own Thread
Safety / Design Goals sections for that (materially more involved)
story. Nothing here changed: every method in *this* doc still runs
entirely on the main thread, start to finish.

## Dependencies

`File.h`, `Path.h` (Phase 3, real-build-confirmed), `JsBindings.md`'s
helpers (Phase 7.2).

## Extensibility

- `fs.readFile`/`fs.writeFile`/etc. (Promise-returning): landed as Phase
  7.5, specified in `FsAsync.md` and now real-build-confirmed through
  7.5.5. Used the bare names reserved by `JsBindings.md`'s `*Sync`-suffix
  convention exactly as planned — no rename was needed.
- `fs.readdirSync`/`fs.statSync`'s fuller shape/symlink support: each
  needs new `forge-core` capability first (see Non-Goals) — natural
  candidates for a later `forge-core` phase, not a rescoping of this doc.
- `net`/`threads`: no fs-specific dependency; both reuse `JsBindings.md`
  directly, not this doc.

## Acceptance Criteria

Each method above implemented and exercised by a real-`mach`-build smoke
test covering at least one success path and one failure path per method
(e.g., `readFileSync` on a real file, and on a nonexistent path,
asserting the caught error's `.code === "NotFound"`). Benchmarked via a
new `bench/fs-bench.js` (Phase 7.4) comparing against Node/Bun's `fs`
equivalents, using the same methodology (`run-benchmarks.ps1`) as every
other script in the suite.

## Implementation Status

**Fully real-build-confirmed (Phase 7.3, 2026-07-30).** All seven
methods above are wired to `globalThis.fs` in `forge.cpp` via
`DefineFsNamespace`, and a 17-case addition to `forge --self-test`
(`RunFsSmokeTests`) covers at least one success and one failure path per
method, per this doc's own Acceptance Criteria above. A real
`python mach build` succeeded and `forge --self-test` printed
`[self-test] ALL PASSED` across all 29 cases (the original 12 Phase 7.2
cases plus these 17), exit code 0 — this doc's entire Public API surface
is real-build-confirmed, not just sandbox-verified. See `ROADMAP.md`'s
Phase 7.3 entry and `HISTORY.md` for the full verification account
(including the sandbox-only compile check and direct-native-function
invocation work done before that real build, for the record).

`bench/fs-bench.js` (Phase 7.4) is written and has been run against real
`forge.exe`/`bun.exe`/`node.exe` on the user's machine (2026-07-30):
Forge/Bun ratio 0.86x, Forge/Node ratio 0.91x, satisfying this doc's own
Acceptance Criteria benchmarking requirement above, with the full
comparison and analysis now in `Forge_Benchmark_Report.md` (authored
2026-07-30, after a prior reference to that file turned out to point at
something that didn't exist on disk — see `ROADMAP.md`/`HISTORY.md`'s
Phase 7.4 entries for that discrepancy and how it was resolved). Phase 7
(all of 7.1-7.4) is now done.
