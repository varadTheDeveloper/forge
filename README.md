<p align="center">
  <img src="assets/forgejs.png" alt="Forge mascot" width="200">
</p>

<h1 align="center">Forge</h1>

Forge is an experimental JavaScript runtime built on Mozilla's SpiderMonkey engine.

The project focuses on building a modern JavaScript runtime from the ground up with a native, high-performance architecture instead of layering new APIs over existing runtimes.

## Current Platform

- ✅ Windows x64

Linux and macOS support are planned.

---

## Running

Create a file:

```javascript
print("Hello Forge!");
```

Run it:

```bash
forge.exe hello.js
```

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

Forge is a much smaller runtime than Bun or Node today, which is part of why it starts faster. Full results and method: [Forge_Benchmark_Report.md](Forge_Benchmark_Report.md).

**Current Highlights**

- 🚀 Fast startup
- 📁 Fastest filesystem API here, sync and async
- ⏱️ Timers level with Bun and Node
- ⚡ Microtasks ahead of Node, behind Bun
- 🎯 Promise chains are the primary optimization target

---

---

## License

MIT
