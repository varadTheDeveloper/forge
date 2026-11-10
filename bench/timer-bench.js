// Phase 6 benchmark: setTimeout scheduling + firing throughput.
// Standard JS (setTimeout is a de facto web/Node/Bun API and Forge's own
// JS-visible builtin, see forge.cpp's SetTimeout/JsTimerRegistry) so this
// runs unmodified on forge, bun, and node alike.
//
// Schedules ITERATIONS independent zero-delay timers up front, then lets
// the event loop drain them. This exercises exactly the code path Phase 6
// changed in Forge: JsTimerRegistry's HashMap<int, UniquePtr<JsTimer>>
// (was std::vector, linearly scanned) for storage/lookup, and
// forge::core::platform::TimerScheduler for due-time bookkeeping
// underneath it (see HISTORY.md/ROADMAP.md's Phase 6 entry).
//
// Deliberately all-zero-delay rather than staggered delays: every timer
// becomes due in the same PopDue() pass, keeping this benchmark's
// wall-clock time bounded by scheduling/dispatch overhead rather than
// real elapsed wait time. A staggered-delay variant (many timers due one
// at a time, spread over real wall-clock time) would be a better stress
// test of TimerScheduler's linear NextDueDelay()/PopDue() scan cost per
// event-loop tick specifically, but was left out here to keep this run
// fast and comparable across all three runtimes — see the optimization
// notes for why that scan is a known architectural risk worth watching
// as pending-timer counts grow.
if (typeof print === "undefined") { var print = console.log; }

// 100,000 since 2026-11-11 (was 5,000, only a few milliseconds of work).
// Not comparable with results before that date.
var ITERATIONS = 100000;
var fired = 0;

function onTimer() {
    fired++;
    if (fired === ITERATIONS) {
        print("timer-bench done, fired=" + fired);
        print("work-ms=" + (Date.now() - BENCH_T0));
    }
}

var BENCH_T0 = Date.now(); // work timer (2026-11-11): run-benchmarks.ps1 reads the "work-ms=" line
for (var i = 0; i < ITERATIONS; i++) {
    setTimeout(onTimer, 0);
}
