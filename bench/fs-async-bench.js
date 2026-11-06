// Phase 7.5.6 benchmark: fs.readFile/writeFile/appendFile/exists/mkdir/
// rm/stat throughput -- the Promise-returning counterparts to
// fs-bench.js's fs.*Sync methods (Phase 7.4). Forge exposes these as
// globalThis.fs (Phase 7.5, now real-build-confirmed through 7.5.5 -- see
// FsAsync.md/HISTORY.md); Node/Bun expose the same names+shapes under
// require("fs").promises, NOT require("fs") directly -- FsAsync.md's own
// Non-Goals rules out a separate fs.promises sub-namespace for Forge
// ("the bare name *is* the Promise-returning variant here"), so the one
// shim line below has to route Node/Bun to their .promises object to get
// a fair, same-shape comparison (require("fs")'s bare fs.readFile is
// Node's callback-style API, a different signature entirely, and would
// silently produce a nonsensical comparison rather than an error).
//
// Deliberately mirrors fs-bench.js's two-pass structure and iteration
// counts (2000 round-trips, 100 "touch every other method" passes) for a
// direct, apples-to-apples read against that benchmark's already-recorded
// numbers -- the only difference this script is trying to isolate is
// "async dispatch through ThreadPool + Promise settlement" vs. "blocking
// call on the main thread", not a different amount of I/O work.
//
// Unlike fs-bench.js's plain for-loop (each blocking call completes
// before the next line runs), every iteration here is chained through
// .then() one step at a time, the same recursive-continuation shape
// promise-chain-bench.js already established in this suite. This isn't
// just style: FsAsync.md's own Non-Goals explicitly rules out any
// ordering guarantee across *concurrent* async fs calls, and this
// benchmark's round-trip pass writes and reads the *same* shared file on
// every iteration -- firing 2000 writes and reads at that one path
// concurrently would be a genuine, not merely theoretical, race (which
// iteration's write does a given read actually observe?), not a fair
// throughput measurement. Chaining serializes them, matching how a real
// caller doing dependent sequential async I/O would write this anyway.
//
// This also happens to run the full 2000+600 chained operations through
// Runtime::run() exactly once per process (the same event-loop-lifetime
// path Phase 7.5.5's Group B self-test isolates and confirms directly),
// so a regression there would likely show up here too as a hang rather
// than a wrong number -- a bonus, not this script's primary purpose.
//
// Same non-cleanup caveat as fs-bench.js: the scratch directory itself is
// never removed (fs.rm is file-removal only, per Fs.md/FsAsync.md's
// shared Non-Goals -- there is no async rmdir any more than there is a
// sync one). Reused, not recreated, across repeated runs.
if (typeof print === "undefined") { var print = console.log; }

var fs;
if (typeof globalThis !== "undefined" && globalThis.fs) {
  fs = globalThis.fs;
} else {
  // Node/Bun's fs.promises has no `.exists` at all (Node deprecated the
  // callback-based fs.exists outright and never added a Promise-based
  // replacement, keeping only the synchronous fs.existsSync) -- unlike
  // every other method here, this isn't a naming mismatch, it's a real
  // capability gap this shim has to paper over to get a fair, actually-
  // runnable comparison against Forge's fs.exists (which does exist, and
  // resolves true/false, never rejects -- see FsAsync.md/Fs.md). Built on
  // fs.promises.access(), the closest real equivalent: access() resolves
  // on success and rejects on failure, so this normalizes that into the
  // same resolves-true/resolves-false shape Forge's own fs.exists uses,
  // rather than leaving the two runtimes' semantics observably different
  // for what's supposed to be the identical benchmark.
  var nodeFsPromises = require("fs").promises;
  fs = Object.assign({}, nodeFsPromises);
  if (typeof fs.exists !== "function") {
    fs.exists = function (path) {
      return nodeFsPromises.access(path).then(
        function () { return true; },
        function () { return false; }
      );
    };
  }
}

var ITERATIONS = 2000;
var SCRATCH_DIR = "fs-async-bench-scratch";
var ROUNDTRIP_PATH = SCRATCH_DIR + "/roundtrip.txt";

// Same payload as fs-bench.js, deliberately -- same reasoning: big enough
// that a write/read isn't measuring pure per-call overhead alone, small
// enough that this stays an API/dispatch-overhead benchmark rather than a
// disk-throughput one.
var PAYLOAD = "The quick brown fox jumps over the lazy dog. ".repeat(8);

var EXTRA_ITERATIONS = 100;
var EXTRA_PATH = SCRATCH_DIR + "/extra.txt";

var checksum = 0;

// Chained write->read round-trip over the one shared ROUNDTRIP_PATH,
// ITERATIONS times. Recurses through .then() rather than a for-loop (see
// header comment) -- each step only runs once the previous one's Promise
// has actually settled, so this never has more than one write or read for
// this path in flight at once.
function roundtripStep(i) {
  if (i >= ITERATIONS) {
    return extraStep(0);
  }

  return fs.writeFile(ROUNDTRIP_PATH, PAYLOAD).then(function () {
    return fs.readFile(ROUNDTRIP_PATH, "utf8");
  }).then(function (text) {
    checksum += text.length;
    return roundtripStep(i + 1);
  });
}

// Lighter pass: touch every remaining method in FsAsync.md's Public API
// at least once per iteration, same set fs-bench.js's own extra pass
// exercises for the sync surface, so this benchmark doubles as an async
// smoke test of the full surface too, not just the round-trip hot path
// above.
function extraStep(j) {
  if (j >= EXTRA_ITERATIONS) {
    return fs.rm(ROUNDTRIP_PATH).then(function () {
      print("fs-async-bench done, iterations=" + ITERATIONS +
            ", extraIterations=" + EXTRA_ITERATIONS + ", checksum=" + checksum);
    });
  }

  return fs.writeFile(EXTRA_PATH, "seed").then(function () {
    return fs.appendFile(EXTRA_PATH, "-append");
  }).then(function () {
    return fs.exists(EXTRA_PATH);
  }).then(function (existsResult) {
    checksum += existsResult ? 1 : 0;
    return fs.stat(EXTRA_PATH);
  }).then(function (stats) {
    checksum += stats.size;
    return fs.rm(EXTRA_PATH);
  }).then(function () {
    return fs.exists(EXTRA_PATH);
  }).then(function (existsResult) {
    checksum += existsResult ? 0 : 1;
    return extraStep(j + 1);
  });
}

fs.mkdir(SCRATCH_DIR, { recursive: true }).then(function () {
  return roundtripStep(0);
}).catch(function (err) {
  // No quit()/process.exit() equivalent exists in Forge today (nothing
  // else in this bench suite needs one, since every prior script is
  // synchronous top to bottom) -- printing a clearly-labeled failure line
  // is the best this script can do on a rejection, same spirit as every
  // other script here having no error handling at all because nothing
  // was expected to fail. run-benchmarks.ps1 will still see exit code 0
  // in this case; a human (or a future harness change) has to notice this
  // line, not an exit code.
  print("fs-async-bench FAILED: " + (err && err.message ? err.message : String(err)));
});
