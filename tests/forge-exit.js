// Expect: exit code 7 (Phase 8). Forge.exit ends the process at once:
// pending timers do not run and finally blocks are skipped.
setTimeout(() => console.log("SHOULD NOT RUN (timer)"), 0);
Promise.resolve().then(() => console.log("SHOULD NOT RUN (microtask)"));
console.log("before exit");
try {
  Forge.exit(7);
} finally {
  console.log("SHOULD NOT RUN (finally)");
}
console.log("SHOULD NOT RUN (after)");
