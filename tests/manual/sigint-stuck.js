// Press Ctrl+C twice. The listener cannot run while the loop is busy, so the
// second Ctrl+C ends the process (exit code 130).
Forge.addSignalListener("SIGINT", () => console.log("listener ran"));
console.log("busy loop with a listener; press Ctrl+C twice");
for (;;) {}
