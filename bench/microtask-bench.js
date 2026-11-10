// Phase 6 benchmark: queueMicrotask scheduling + drain throughput.
// queueMicrotask is a standard web/Node/Bun API and one of Forge's own
// JS-visible builtins (see forge.cpp's QueueMicrotask/EnqueueMicrotask),
// so this runs unmodified on forge, bun, and node alike.
//
// Schedules ITERATIONS microtasks up front (same iteration count as
// json-bench.js for rough cross-benchmark comparability), each just
// incrementing a counter. Exercises exactly the container Phase 6
// changed in Forge: the microtask queue itself
// (Queue<UniquePtr<Microtask>>, was std::vector<std::unique_ptr<Microtask>>
// erased from the front every drain — see HISTORY.md's Phase 6 entry).
if (typeof print === "undefined") { var print = console.log; }

// 1,000,000 since 2026-11-11 (was 200,000). Not comparable with results
// before that date.
var ITERATIONS = 1000000;
var ran = 0;

var BENCH_T0 = Date.now(); // work timer (2026-11-11): run-benchmarks.ps1 reads the "work-ms=" line
for (var i = 0; i < ITERATIONS; i++) {
    queueMicrotask(function () {
        ran++;
        if (ran === ITERATIONS) {
            print("microtask-bench done, ran=" + ran);
            print("work-ms=" + (Date.now() - BENCH_T0));
        }
    });
}
