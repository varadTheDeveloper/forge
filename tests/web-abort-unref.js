// Phase 10.3: AbortSignal.timeout's timer does not keep the program
// running (as in Node and Deno). The program ends once the ordinary timer
// has run, long before the signal would time out; nothing is printed for
// the signal. tests/run-runtime-tests.ps1 also checks how long it took.

const signal = AbortSignal.timeout(60000);
signal.onabort = () => console.log("SHOULD NOT FIRE");
AbortSignal.any([signal]).addEventListener("abort", () => console.log("SHOULD NOT FIRE either"));
setTimeout(() => console.log("ordinary timer ran"), 20);
console.log("unref start");
