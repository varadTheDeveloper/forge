// Expect: exit code 1 (Phase 8): a thrown non-Error value is printed with
// its stack, quoted like any other value.
function thrower() {
  throw { reason: "not an Error" };
}
thrower();
