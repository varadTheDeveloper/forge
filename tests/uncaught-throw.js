// Expect: exit code 1, the error and its stack on stderr.
function inner() {
  throw new Error("boom from main script");
}
inner();
print("SHOULD NOT RUN");
