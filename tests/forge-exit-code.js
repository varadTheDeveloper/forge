// Expect: exit code 5 (Phase 8). Forge.exitCode is used when the script
// ends normally, after pending timers have run.
Forge.exitCode = 5;
setTimeout(() => console.log("timer ran"), 10);
