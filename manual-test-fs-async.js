// Manual verification script for Phase 7.5.3/7.5.4 (all seven
// Promise-returning async fs bindings) -- not part of `forge --self-test`.
//
// Superseded (2026-08-02): Phase 7.5.5 added a genuine Promise-based
// self-test harness (RunFsAsyncSmokeTests in forge.cpp, run from
// `forge --self-test`) that is now the feature's ongoing regression
// check -- see ROADMAP.md/HISTORY.md's 7.5.5 entries. This script was
// the interim way to confirm the whole pipeline worked end to end on the
// real machine before that harness existed (used for 7.5.3 and 7.5.4's
// own real-build confirmations), and is kept here for reference rather
// than deleted, but new verification should go through
// `forge --self-test` instead of this file.
//
// Run via:
//   forge manual-test-fs-async.js
//
// This script's only pending work, once its body finishes running, is
// the Promises below -- there is no setTimeout/setInterval anywhere in
// this file. That deliberately also exercises the Phase 7.5.2 event-loop
// lifetime fix: without the pendingAsyncOps_ counter, Forge's run()
// would see an empty job queue and an empty IoLoop (no timers pending)
// and exit immediately, before any of these ever got a chance to
// complete.

let failures = 0;

function check(condition, passMessage, failMessage) {
  if (condition) {
    print("PASS: " + passMessage);
  } else {
    failures++;
    print("FAIL: " + failMessage);
  }
}

//------------------------------------------------------------------------
// Phase 7.5.3: fs.readFile -- both encodings' success paths, and the
// NotFound rejection path. These three are independent of each other and
// of everything below, so they run concurrently rather than chained.
//------------------------------------------------------------------------

const readPath = "forge-selftest-fs-scratch/async-read-test.txt";
const readContents = "hello from phase 7.5.3";

fs.writeFileSync(readPath, readContents);

fs.readFile(readPath, "utf8").then(
  (text) => {
    check(text === readContents,
          "fs.readFile(path, \"utf8\") resolved with the correct text",
          "fs.readFile(path, \"utf8\") resolved with unexpected text: " + JSON.stringify(text));
  },
  (err) => {
    failures++;
    print("FAIL: fs.readFile(path, \"utf8\") unexpectedly rejected: " + JSON.stringify(err));
  }
);

fs.readFile(readPath).then(
  (bytes) => {
    check(bytes instanceof Uint8Array && bytes.length === readContents.length,
          "fs.readFile(path) resolved with a Uint8Array of the right length",
          "fs.readFile(path) resolved with something unexpected");
  },
  (err) => {
    failures++;
    print("FAIL: fs.readFile(path) unexpectedly rejected: " + JSON.stringify(err));
  }
);

fs.readFile("forge-selftest-fs-scratch/definitely-does-not-exist.txt").then(
  () => {
    failures++;
    print("FAIL: fs.readFile should have rejected for a missing file, but resolved");
  },
  (err) => {
    check(err && err.code === "NotFound",
          "fs.readFile rejected with .code === \"NotFound\" for a missing file",
          "fs.readFile rejected, but not with the expected .code: " + JSON.stringify(err));
  }
);

//------------------------------------------------------------------------
// Phase 7.5.4: the remaining six methods. Each one's outcome depends on
// the file state the previous step left behind, so these are chained
// through .then() rather than fired concurrently -- fs.writeFile and
// fs.rm racing over the same path would otherwise be a real, if
// deliberately out of scope (FsAsync.md's Non-Goals: no ordering
// guarantee across *concurrent* calls), way to get a flaky result here.
//------------------------------------------------------------------------

const miscPath = "forge-selftest-fs-scratch/async-misc-test.txt";
const miscDir = "forge-selftest-fs-scratch/async-mkdir-test/nested";

fs.writeFile(miscPath, "first").then(() => {
  check(fs.existsSync(miscPath) && fs.readFileSync(miscPath, "utf8") === "first",
        "fs.writeFile resolved and wrote the expected content",
        "fs.writeFile resolved, but the file content is wrong");

  return fs.appendFile(miscPath, "-second");
}).then(() => {
  check(fs.readFileSync(miscPath, "utf8") === "first-second",
        "fs.appendFile resolved and appended the expected content",
        "fs.appendFile resolved, but the file content is wrong");

  return fs.stat(miscPath);
}).then((stats) => {
  const expectedSize = "first-second".length;
  check(stats && stats.size === expectedSize,
        "fs.stat resolved with the correct size (" + expectedSize + ")",
        "fs.stat resolved with an unexpected shape: " + JSON.stringify(stats));

  return fs.exists(miscPath);
}).then((existsResult) => {
  check(existsResult === true,
        "fs.exists resolved true for a file that exists",
        "fs.exists resolved " + JSON.stringify(existsResult) + " for a file that exists");

  return fs.rm(miscPath);
}).then(() => {
  check(!fs.existsSync(miscPath),
        "fs.rm resolved and the file is actually gone",
        "fs.rm resolved, but the file still exists");

  return fs.exists(miscPath);
}).then((existsResult) => {
  check(existsResult === false,
        "fs.exists resolved false for a file that no longer exists",
        "fs.exists resolved " + JSON.stringify(existsResult) + " for a removed file");

  return fs.mkdir(miscDir, { recursive: true });
}).then(() => {
  // Fs.md's Non-Goals rule out any isDirectory()/directory-listing
  // capability, so existsSync can't directly confirm a *directory* was
  // created -- instead, write a real file inside the new nested
  // directory and confirm that succeeds, which is only possible if
  // fs.mkdir(recursive) actually created it.
  let placeholderWorked = false;
  try {
    fs.writeFileSync(miscDir + "/placeholder.txt", "x");
    placeholderWorked = fs.existsSync(miscDir + "/placeholder.txt");
  } catch (e) {
    placeholderWorked = false;
  }

  check(placeholderWorked,
        "fs.mkdir(recursive) resolved and the nested directory actually exists",
        "fs.mkdir(recursive) resolved, but the nested directory doesn't seem to exist");

  // fs.mkdir on an already-existing directory, non-recursive, should
  // reject with AlreadyExists -- mirrors mkdirSync's own documented
  // non-idempotent case (Phase 7.3's SelfTest_FsMkdirSyncNonRecursive).
  return fs.mkdir(miscDir).then(
    () => {
      failures++;
      print("FAIL: fs.mkdir (non-recursive) on an existing directory should have rejected");
    },
    (err) => {
      check(err && err.code === "AlreadyExists",
            "fs.mkdir (non-recursive) rejected with .code === \"AlreadyExists\" as expected",
            "fs.mkdir (non-recursive) rejected, but not with the expected .code: " + JSON.stringify(err));
    }
  );
}).then(() => {
  return fs.readFile("forge-selftest-fs-scratch/still-does-not-exist.txt").then(
    () => {
      failures++;
      print("FAIL: fs.readFile should have rejected for a missing file, but resolved");
    },
    () => {
      // Already covered above; this second check just confirms the
      // whole chain above ran to completion without an uncaught
      // rejection silently stopping it partway through.
      print("Chained fs.* async sequence ran to completion.");
    }
  );
}).then(() => {
  print("");
  print(failures === 0
      ? "ALL CHECKS PASSED"
      : failures + " CHECK(S) FAILED");
});

print("Script body finished. If the Phase 7.5.2 event-loop lifetime fix");
print("works, the process now waits for the async operations above to");
print("complete (and prints their PASS/FAIL lines) instead of exiting");
print("immediately.");
