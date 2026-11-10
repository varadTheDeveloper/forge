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

Median of 7 runs on one Windows x64 machine, 2026-11-10. Forge is built on SpiderMonkey ESR 153.

**Startup** (run a one-line script): Forge **17 ms**, Bun 48 ms, Node 48 ms.

**Work time**: total time minus each runtime's own startup time. This is an estimate; lower is better.

| Benchmark | Forge | Bun | Node | Winner |
|-----------|------:|----:|-----:|:------:|
| JSON | 389 ms | **220 ms** | 290 ms | 🥇 Bun |
| Loop | 51 ms | **30 ms** | 50 ms | 🥇 Bun |
| Timers | **2 ms** | 9 ms | 11 ms | 🥇 Forge |
| Microtasks | 25 ms | **14 ms** | 62 ms | 🥇 Bun |
| Promise Chains | 15 ms | **4 ms** | 6 ms | 🥇 Bun |
| Filesystem (sync) | 1,007 ms | 1,111 ms | 997 ms | Tie: Node, Forge |
| Filesystem (async) | **1,040 ms** | 1,308 ms | 1,470 ms | 🥇 Forge |

Forge is a much smaller runtime than Bun or Node today, which is part of why it starts faster. The full numbers, including total times, and the method are in [Forge_Benchmark_Report.md](Forge_Benchmark_Report.md).

**Current Highlights**

- 🚀 Fast startup
- ⏱️ High-performance timers
- 📁 Fast filesystem API, sync and async
- ⚡ Microtasks ahead of Node, behind Bun
- 🎯 Promise chains are the primary optimization target

---

---

## License

MIT
