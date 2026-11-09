// Expect: exit code 1. The throwing microtask must not be re-run forever,
// and nothing runs after the fatal error.
queueMicrotask(() => {
  throw new Error("boom from microtask");
});
queueMicrotask(() => print("SHOULD NOT RUN"));
