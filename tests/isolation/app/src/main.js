// Phase 9.3: the entry module of the isolation runtime test. Run with no
// permissions; see ../../README.md for what each package tests.
import * as probe from "probe";
import data from "probe/data" with { type: "json" };
import { count, bump } from "live";

const appSecret = "app-secret";  // a package must not see this binding
const lines = [];
const log = (text) => lines.push(text);

for (const line of probe.checks) {
  log("probe " + line);
}

log("meta in a package: " + probe.metaOk);
log("json across packages: " + data.name + " " + data.items.length);

// The program's own built-ins are untouched by the package.
log("app not polluted: " + (({}).polluted === undefined) + " " + !Object.isFrozen(Object.prototype));

// Ordinary values cross, as wrappers.
log("closure crosses: " + probe.callBack(() => "app-closure"));
log("data crosses: " + probe.identity([1, 2, 3]).length + " " + typeof appSecret);

// An intrinsic is replaced by the receiving side's own.
log("pollute through a wrapper: " + probe.pollute({}));
log("app not polluted after: " + (({}).polluted === undefined));
log("prototypes map: " + probe.protoIsOwn({}, [1]));
log("intrinsics map back: " + (probe.identity(Function) === Function) + " " +
  (probe.identity(Object.prototype) === Object.prototype));
log("Function through constructor: " + probe.reachFunction({}));

// Forge's own objects, functions and the global object never cross.
function refused(label, value) {
  try {
    probe.identity(value);
    log("SHOULD NOT CROSS " + label);
  } catch (e) {
    log("refused " + label + ": " + e.code);
  }
}

refused("fs", fs);
refused("fs function", fs.readFileSync);
refused("bound fs function", fs.readFileSync.bind(fs));
refused("console", console);
refused("console.log", console.log);
refused("Forge", Forge);
refused("Forge.env.get", Forge.env.get);
refused("global object", globalThis);
refused("import.meta.resolve", import.meta.resolve);
log("fs inside an object: " + probe.readProperty({ fs }, "fs"));
log("Forge.args crosses: " + Array.isArray(probe.identity(Forge.args)));

// Errors cross and keep their type.
try {
  probe.fail();
} catch (e) {
  log("error crosses: " + (e instanceof TypeError) + " " + e.message);
}

// RegExp statics are per compartment.
/token-(\d+)/.exec("token-123");
log("regexp statics separate: " + (probe.regexpLastMatch() !== "token-123") + " " +
  (RegExp.lastMatch === "token-123"));

// Snapshot semantics across compartments (a documented deviation).
log("live binding: bump=" + bump() + " seen=" + count);

// Logging a package's object prints the object, not "[Proxy]".
console.log(probe.sample);

// Timers and microtasks belong to the realm that made them.
let appTimerRan = false;
const appTimer = setTimeout(() => { appTimerRan = true; }, 20);
log("clear from a package: " + probe.clear(appTimer));
log("package timer: " + await probe.later("later-ok"));
log("package microtask: " + await probe.micro(() => "micro-ok"));
await new Promise((resolve) => probe.crossMicro(() => { log("wrapped microtask ran"); resolve(); }));
await new Promise((resolve) => setTimeout(resolve, 60));
log("app timer survived: " + appTimerRan);

// Dynamic imports: inside a package, of a throwing package, of a cycle.
log("package dynamic import: " + await probe.loadHelper());
log("spread import: " + await probe.spreadImport());

try {
  await import("thrower");
  log("SHOULD NOT LOAD thrower");
} catch (e) {
  log("thrower: " + (e instanceof TypeError) + " " + e.message);
}

try {
  await import("cycle-a");
  log("SHOULD NOT LOAD cycle");
} catch (e) {
  log("cycle: " + e.code + " " + e.message.includes("cycle-a -> cycle-b -> cycle-a"));
}

console.log(lines.join("\n"));
console.log("isolation done");
