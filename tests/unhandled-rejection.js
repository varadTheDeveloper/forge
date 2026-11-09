// Expect: exit code 1 and "Uncaught (in promise)" on stderr.
function reject() {
  return Promise.reject(new Error("rejected and never handled"));
}
reject();
