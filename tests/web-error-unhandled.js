// Phase 10.2: an error listener that does not cancel: the error is still
// uncaught (exit code 1), after the listener ran. An exception thrown by an
// error listener itself is uncaught at once, with no second event.
addEventListener("error", (e) => {
  console.log("listener saw: " + e.error.message);

  if (e.error.message === "listener boom") {
    throw new Error("thrown inside the error listener");
  }
});

const target = new EventTarget();
target.addEventListener("x", () => { throw new Error("listener boom"); });
target.dispatchEvent(new Event("x"));
console.log("after dispatch");
setTimeout(() => console.log("SHOULD NOT RUN"), 0);
