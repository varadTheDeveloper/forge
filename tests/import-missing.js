// Phase 9.1: a static import of a file that does not exist stops the
// program before any of its code runs (exit code 1).
import "./does-not-exist.js";

console.log("SHOULD NOT RUN");
