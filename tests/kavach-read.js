// Phase 8.2: reads Forge.args[0] (inside the read grant) and tries
// Forge.args[1] (outside it, possibly through a junction). Expect the first
// to print and the second to be refused with PermissionDenied.
const [allowed, denied] = Forge.args;
console.log("allowed: " + fs.readFileSync(allowed, "utf8"));
try {
  fs.readFileSync(denied, "utf8");
  console.log("SHOULD NOT READ " + denied);
} catch (e) {
  console.log("denied: " + e.code + " " + e.permission);
}
