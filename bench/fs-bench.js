// Phase 7.4 benchmark: fs.*Sync read/write/append/exists/mkdir/rm/stat
// throughput. Forge exposes these as globalThis.fs (Phase 7.3, now
// real-build-confirmed -- see Fs.md); Node/Bun expose the same shape via
// require("fs") (Fs.md's Design Goals deliberately mirror Node's own
// naming/defaults for exactly this reason), so one shim line makes this
// script runnable unmodified on all three -- closing the "filesystem
// operations" gap every benchmark report since Phase 0 has had to flag as
// not-yet-benchmarkable (see run-benchmarks.ps1's former scope note, now
// updated to reflect this script's existence).
//
// Two passes, deliberately weighted differently:
//   1. A write/read round-trip loop -- the hot path most real fs usage
//      actually spends time in -- reusing one file across all ITERATIONS
//      iterations (not one file per iteration), so this measures per-call
//      API/syscall overhead, not filesystem-metadata or directory-entry
//      growth.
//   2. A single lighter pass over appendFileSync/existsSync/mkdirSync/
//      rmSync/statSync -- enough to exercise every remaining method in
//      Fs.md's Public API at least once, without pretending these are
//      equally hot-path operations (they're not, in any real workload).
//
// Deliberately does NOT remove fs-bench-scratch (the directory itself)
// when done: Fs.md's own Non-Goals section is explicit that directory
// removal is out of scope for Phase 7 (File::Remove's Win32 backend is
// DeleteFileW, which cannot remove a directory) -- so there is nothing in
// today's fs.*Sync surface this script could call to clean that up even
// if it wanted to. The scratch dir is reused (not recreated) on every
// run, so this doesn't grow unbounded across repeated benchmark runs.
if (typeof print === "undefined") { var print = console.log; }
var fs = (typeof globalThis !== "undefined" && globalThis.fs) ? globalThis.fs : require("fs");

var ITERATIONS = 2000;
var SCRATCH_DIR = "fs-bench-scratch";
var ROUNDTRIP_PATH = SCRATCH_DIR + "/roundtrip.txt";

// A few hundred bytes -- big enough that the write/read isn't measuring
// pure syscall overhead alone, small enough that ITERATIONS runs don't
// turn this into a disk-throughput benchmark instead of an API-overhead
// one.
var PAYLOAD = "The quick brown fox jumps over the lazy dog. ".repeat(8);

fs.mkdirSync(SCRATCH_DIR, { recursive: true });
var BENCH_T0 = Date.now(); // work timer (2026-11-11): run-benchmarks.ps1 reads the "work-ms=" line

var checksum = 0;

for (var i = 0; i < ITERATIONS; i++) {
    fs.writeFileSync(ROUNDTRIP_PATH, PAYLOAD);
    var text = fs.readFileSync(ROUNDTRIP_PATH, "utf8");
    checksum += text.length;
}

// Lighter pass: touch every remaining method in Fs.md's Public API at
// least once, so this benchmark doubles as a smoke test of the full
// surface, not just the round-trip hot path above.
var EXTRA_ITERATIONS = 100;
var EXTRA_PATH = SCRATCH_DIR + "/extra.txt";

for (var j = 0; j < EXTRA_ITERATIONS; j++) {
    fs.writeFileSync(EXTRA_PATH, "seed");
    fs.appendFileSync(EXTRA_PATH, "-append");
    checksum += fs.existsSync(EXTRA_PATH) ? 1 : 0;
    checksum += fs.statSync(EXTRA_PATH).size;
    fs.rmSync(EXTRA_PATH);
    checksum += fs.existsSync(EXTRA_PATH) ? 0 : 1;
}

fs.rmSync(ROUNDTRIP_PATH);

print("fs-bench done, iterations=" + ITERATIONS + ", extraIterations=" + EXTRA_ITERATIONS + ", checksum=" + checksum);
print("work-ms=" + (Date.now() - BENCH_T0));
