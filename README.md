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

| Benchmark | Forge | Bun | Node | Winner |
|-----------|------:|----:|-----:|:------:|
| Startup | **19.65 ms** | 38.93 ms | 43.62 ms | 🥇 Forge |
| JSON | 368.79 ms | **249.93 ms** | 546.43 ms | 🥇 Bun |
| Loop | 81.98 ms | **71.56 ms** | 90.95 ms | 🥇 Bun |
| Timers | **24.13 ms** | 49.44 ms | 59.55 ms | 🥇 Forge |
| Microtasks | 55.93 ms | **54.71 ms** | 108.69 ms | 🥇 Bun |
| Promise Chains | 88.84 ms | **44.23 ms** | 48.02 ms | 🥇 Bun |
| Filesystem | **810.60 ms** | 990.25 ms | 918.78 ms | 🥇 Forge |

**Current Highlights**

- 🚀 Fast startup
- ⏱️ High-performance timers
- 📁 Fast synchronous filesystem API
- ⚡ Microtasks close to Bun
- 🎯 Promise chains are the primary optimization target

---

---

## License

MIT
