// Phase 9.1: a top-level await that rejects is an uncaught exception
// (exit code 1), reported after the module has started running.
console.log("before await");
await null;
throw new Error("late boom from top-level await");
