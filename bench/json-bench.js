// Phase 0 benchmark: JSON parse/stringify throughput.
// Uses only standard JS (JSON is a built-in ECMAScript global provided by
// the engine itself, not something Forge has to implement) so this runs
// unmodified on forge, bun, and node alike.
if (typeof print === "undefined") { var print = console.log; }

var ITERATIONS = 200000;

var sample = {
    id: 12345,
    name: "Forge Benchmark",
    active: true,
    tags: ["fast", "native", "js-runtime"],
    nested: { a: 1, b: 2.5, c: "text", d: [1, 2, 3, 4, 5] }
};

var BENCH_T0 = Date.now(); // work timer (2026-11-11): run-benchmarks.ps1 reads the "work-ms=" line
var checksum = 0;

for (var i = 0; i < ITERATIONS; i++) {
    var text = JSON.stringify(sample);
    var parsed = JSON.parse(text);
    checksum += parsed.nested.d.length;
}

print("json-bench done, iterations=" + ITERATIONS + ", checksum=" + checksum);
print("work-ms=" + (Date.now() - BENCH_T0));
