// Phase 0 benchmark: cold-start cost.
// Deliberately does almost nothing — the whole point is to measure how
// long the runtime itself takes to boot (engine init, global setup,
// parsing this one line) before any user code would even start running.
// Timed externally (wall-clock around the whole process), not internally.
//
// Forge has a global print(); Node/Bun don't by default (they use
// console.log). This one-line shim means this exact file runs unmodified
// on all three runtimes, so the comparison is fair.
if (typeof print === "undefined") { var print = console.log; }

var BENCH_T0 = Date.now();
print("ready");
print("work-ms=" + (Date.now() - BENCH_T0));
