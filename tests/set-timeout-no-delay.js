// Expect: exit code 0 and "fired" on stdout. setTimeout(fn) used to end
// the script silently because the delay argument was required.
setTimeout(() => print("fired"));
