// Expect: exit code 1 (Phase 8): a thrown non-Error value is printed with
// its stack, quoted like any other value. Phase 9.1: thrown from a timer,
// because SpiderMonkey keeps only the value (not the throw site) when a
// module's top-level code throws (PHASE9_DESIGN.md, section 3, "Errors").
function thrower() {
  throw { reason: "not an Error" };
}
setTimeout(thrower, 0);
