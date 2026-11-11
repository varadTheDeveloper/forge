// Phase 8.2: no permission was granted, so this read is refused and, being
// uncaught, ends the program with exit code 1 and the refusal message.
fs.readFileSync(Forge.args[0], "utf8");
console.log("SHOULD NOT RUN");
