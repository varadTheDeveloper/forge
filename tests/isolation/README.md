# Isolation test tree (Phase 9.3)

`run-runtime-tests.ps1` copies `app/` into a temporary folder and runs
`app/src/main.js` with no permissions. Every package here holds no
permission, so all of them run in the shared, frozen "pure" compartment
(PHASE9_DESIGN.md, section 5).

| Package | What it tests |
|---|---|
| `probe` | From inside a package: no `fs`, `Forge` or `print`; `eval`, indirect `eval` and `new Function` refused; frozen intrinsics (also the hidden ones), global object and `console`; the override mistake handled; the program's top-level bindings invisible; no realm-making API. Its functions are called by the program to test the wrapper policy: an intrinsic replaced by the receiving side's own, Forge's objects and functions, bound functions, `import.meta.resolve` and the global object refused, the `Function` constructor reached through `obj.constructor.constructor`, errors crossing with their type, RegExp statics per compartment, timers and microtasks across compartments, a dynamic import inside a package, and a spread `import()` (LavaMoat's CVE-2023-39532). It also exports a JSON file (`probe/data`). |
| `@iso/helper` | A dependency of `probe`, imported dynamically from inside it. |
| `live` | Snapshot semantics: a later reassignment of an `export let` is not seen by another package. |
| `thrower` | A package that throws at its top level: the importer gets the package's error, still a `TypeError`. |
| `cycle-a`, `cycle-b` | An import cycle between packages is refused at load time, naming both. |
| `webby` | Phase 10.1: the web APIs (`DOMException`, `TextEncoder`, `TextDecoder`, `atob`, `btoa`) exist and are frozen inside a package. Their classes cross as intrinsics (the receiving side's own); instances cross as wrappers, with getters, `instanceof`, `toString` and console output working, and a method called on another compartment's instance (`decode`, `encodeInto`, a getter through `.call`) forwarded to that compartment, both ways. |

`tests/frozen-intrinsics.js` runs a program with `--frozen-intrinsics`.
