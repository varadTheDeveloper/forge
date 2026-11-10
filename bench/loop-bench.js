// Phase 0 benchmark: raw JS execution (engine-bound).
// This one mostly measures the JS engine itself (SpiderMonkey vs
// JavaScriptCore vs V8), not anything Forge's own runtime layer controls.
// Track it anyway — it's an honest baseline, not a target Forge can move
// much on its own.
if (typeof print === "undefined") { var print = console.log; }

function fib(n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

// fib(36) since 2026-11-11 (was fib(34)): enough work that startup time
// does not dominate. Not comparable with results before that date.
var BENCH_T0 = Date.now(); // work timer (2026-11-11): run-benchmarks.ps1 reads the "work-ms=" line
var result = fib(36);
print("loop-bench done, fib(36)=" + result);
print("work-ms=" + (Date.now() - BENCH_T0));
