// Expect: exit code 0; prints the script's arguments (Phase 8). The runner
// passes "a b", "--flag" and "ü" (u with umlaut) after the script path.
console.log("args: " + JSON.stringify(Forge.args));
console.log("count: " + Forge.args.length);
