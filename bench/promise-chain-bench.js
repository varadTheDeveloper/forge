// Phase 6 benchmark: sequential Promise.then() chaining.
// Promise is a standard ECMAScript global the JS engine itself provides
// (SpiderMonkey/V8/JavaScriptCore alike, via JS::InitRealmStandardClasses
// in Forge's case) -- Forge didn't have to implement Promise itself, only
// wire JS::SetJobQueue(cx, &jobQueue_) so promise reactions actually get
// scheduled (see forge.cpp's ForgeJobQueue::enqueuePromiseJob). This runs
// unmodified on forge, bun, and node alike.
//
// Unlike microtask-bench.js (which schedules every microtask up front),
// this chains ITERATIONS .then() continuations one at a time -- each only
// scheduled once the previous one resolves. That's a much closer shape to
// real async/await-style code (a sequence of dependent async steps) than
// a flat burst of independent microtasks, and stresses the one-at-a-time
// schedule/dispatch/reschedule loop rather than a queue's raw throughput
// under a single big backlog.
if (typeof print === "undefined") { var print = console.log; }

// 1,000,000 since 2026-11-11 (was 50,000, only 4-15 ms of work).
// Not comparable with results before that date.
var ITERATIONS = 1000000;
var count = 0;

function step() {
    count++;
    if (count >= ITERATIONS) {
        print("promise-chain-bench done, count=" + count);
        print("work-ms=" + (Date.now() - BENCH_T0));
        return;
    }
    Promise.resolve().then(step);
}

var BENCH_T0 = Date.now(); // work timer (2026-11-11): run-benchmarks.ps1 reads the "work-ms=" line
Promise.resolve().then(step);
