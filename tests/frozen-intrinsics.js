// Phase 9.3: run with --frozen-intrinsics. The program's own built-in
// objects are frozen as in a package's compartment, but the program keeps
// its natives and may still use eval.
const results = [];

results.push("frozen: " + Object.isFrozen(Object.prototype) + " " + Object.isFrozen(Array.prototype));

try {
  Array.prototype.extra = 1;
  results.push("SHOULD NOT EXTEND Array.prototype");
} catch (e) {
  results.push("extend: " + e.name);
}

// The "override mistake" is handled.
class Fancy extends Error {
  constructor(message) {
    super(message);
    this.name = "Fancy";
  }
}
const plain = {};
plain.toString = () => "ok";
results.push("override: " + new Fancy("x").name + " " + String(plain));

results.push("app globals: " + typeof fs + " " + typeof Forge);
results.push("eval in the program: " + eval("1 + 1"));

console.log(results.join("\n"));
console.log("frozen done");
