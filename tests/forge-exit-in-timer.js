// Expect: exit code 3 (Phase 8). Forge.exit inside a timer callback stops
// everything else that was pending.
setTimeout(() => {
  console.log("first timer");
  Forge.exit(3);
}, 0);
setTimeout(() => console.log("SHOULD NOT RUN"), 50);
