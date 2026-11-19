<p align="center">
  <img src="assets/forgejs.png" alt="Forge mascot" width="200">
</p>

<h1 align="center">Forge</h1>

Forge™ is an experimental JavaScript runtime built on Mozilla's SpiderMonkey engine.

The project focuses on building a modern JavaScript runtime from the ground up with a native, high-performance architecture instead of layering new APIs over existing runtimes.

Its goal is security: **a program can touch nothing outside itself unless you allow it**, and later each package will get only the permissions it declares. Forge's permission system is called **Kavach™**.

## Current Platform

- ✅ Windows x64

Linux and macOS support are planned.

---

## Running

Create a file:

```javascript
console.log("Hello Forge!");
console.log(fs.readFileSync("data/notes.txt", "utf8"));
```

Run it, allowing it to read the `data` folder and nothing else:

```bash
forge.exe --allow-read=data hello.js
```

Without `--allow-read`, the file read is refused with a message that names the exact option that would allow it.

Every program is an ES module, so `import`, top-level `await`, JSON modules (`with { type: "json" }`) and `import()` work. A program loads its own modules without any permission, but only from its own folder (the nearest folder with a `forge.json`, or the folder of the file you run); nothing is ever loaded from the network.

Packages are folders with a `forge.json`. A program imports a package by name only if its `forge.json` lists it in `"dependencies"` (as a local `file:` folder for now), and only what the package lists in `"exports"`; reaching into a package by path is refused. Each package runs isolated from the program, with frozen built-ins and no access at all (no `fs`, no `Forge`, no `eval`); Forge's own functions never cross into it. A package gets a permission only if its `forge.json` declares it, the program's `forge.json` grants it (`"grants"`), and the command line allows it; then it gets `fs` and `Forge.env` bound to its own permissions. Details: [Kavach.md](Kavach.md) and [PHASE9_DESIGN.md](PHASE9_DESIGN.md).

---

## Security: Kavach

- **Deny by default.** Files, folders and environment variables are refused unless granted with `--allow-read=`, `--allow-write=` or `--allow-env=`.
- **Hard to trick.** Paths are put in one canonical form; Windows' ambiguous path forms are refused; links and junctions are followed before matching; every file operation is re-checked on the opened handle.
- **Protected paths.** `.git`, `forge.json`, `forge.lock`, the audit log and Forge's install folder can never be granted, and neither can a whole drive or your home folder.
- **Audit log.** `--audit-log=<file>` records every decision as JSON lines.

- **Tested against known bypasses.** Published bypasses of other runtimes' permission systems are replayed against Kavach on every build ([BYPASS-REPLAYS.md](BYPASS-REPLAYS.md)), and every native function is fuzzed.

Status: **preview.** Per-package permissions come with modules (Phase 9) and an OS sandbox later. Details and known limits: [Kavach.md](Kavach.md). TypeScript declarations: [types/forge.d.ts](types/forge.d.ts).

---

## Standards

Forge targets the [WinterTC Minimum Common Web API](https://min-common-api.proposal.wintertc.org/), the standard for server-side JavaScript interoperability developed by Ecma TC55. **Forge does not conform yet.** Phase 10 has begun: `DOMException`, `TextEncoder`, `TextDecoder` (UTF-8, UTF-16 and every single-byte encoding), `atob`/`btoa`, `EventTarget` and the event classes, `AbortController` and `AbortSignal`, `structuredClone` and `MessageChannel`, `reportError`, `self` and `navigator` work, in packages too, and an uncaught error or unhandled rejection is first an `error` or `unhandledrejection` event that the program can cancel; the rest of the required APIs arrive in Phases 10 and 11 ([PHASE10_DESIGN.md](PHASE10_DESIGN.md)). The entry-by-entry status is in [WinterTC.md](WinterTC.md).

---

## Benchmarks

Median of 7 runs, 2026-11-10, on an AMD Ryzen 5 5600X with Windows 11. Forge v0.1.0 on SpiderMonkey ESR 153, Bun 1.3.14, Node 24.11.1.

**Startup** (run a one-line script): Forge **15 ms**, Bun 40 ms, Node 43 ms.

**Work time**, measured inside each script, so startup is not included. Lower is better.

| Benchmark | Forge | Bun | Node | Winner |
|-----------|------:|----:|-----:|:------:|
| JSON (200k round trips) | 361 ms | **209 ms** | 286 ms | 🥇 Bun |
| Loop (`fib(36)`) | 131 ms | **82 ms** | 129 ms | 🥇 Bun |
| Timers (100k) | 40 ms | 43 ms | **35 ms** | 🥇 Node |
| Microtasks (1M) | 122 ms | **54 ms** | 254 ms | 🥇 Bun |
| Promise chains (1M steps) | 246 ms | 60 ms | **32 ms** | 🥇 Node |
| Filesystem, sync | **1,609 ms** | 1,908 ms | 1,674 ms | 🥇 Forge |
| Filesystem, async | **1,683 ms** | 1,946 ms | 2,050 ms | 🥇 Forge |

Forge is a much smaller runtime than Bun or Node today, which is part of why it starts faster. These numbers were measured before Kavach's permission checks were added (Phase 8.2 and 8.3); the filesystem results will be re-measured. Full results and method: [Forge_Benchmark_Report.md](Forge_Benchmark_Report.md).

**Current Highlights**

- 🚀 Fast startup
- 📁 Fastest filesystem API here, sync and async
- ⏱️ Timers level with Bun and Node
- ⚡ Microtasks ahead of Node, behind Bun
- 🎯 Promise chains are the primary optimization target

---

## License

MIT

---

Forge™ and Kavach™ are trademarks used by the Forge project.
