// Expect: exit code 0 and "caught: handled" on stdout.
const p = Promise.reject(new Error("handled"));
p.catch((e) => print("caught: " + e.message));
