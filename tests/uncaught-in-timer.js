// Expect: exit code 1; nothing runs after the fatal error.
setTimeout(() => {
  throw new Error("boom from timer");
}, 0);
setTimeout(() => print("SHOULD NOT RUN"), 50);
