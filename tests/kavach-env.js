// Phase 8.2: FORGE_KAVACH_TEST is granted with --allow-env; USERNAME is not.
console.log("env: " + Forge.env.get("FORGE_KAVACH_TEST"));
try {
  Forge.env.get("USERNAME");
  console.log("SHOULD NOT READ USERNAME");
} catch (e) {
  console.log("env denied: " + e.code);
}
