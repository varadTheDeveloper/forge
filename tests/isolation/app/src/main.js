// Phase 9.3: the entry module of the isolation runtime test. Run with no
// permissions; see ../../README.md for what each package tests.
import * as probe from "probe";
import data from "probe/data" with { type: "json" };
import { count, bump } from "live";
import * as webby from "webby";

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

// Phase 10.1: web API objects across compartments. The classes are
// intrinsics (one crossing becomes the other side's own); instances cross
// as wrappers, and a method called on another compartment's instance is
// forwarded to that compartment (runtime/WebApis.cpp, NativeForward).
for (const line of webby.checks) {
  log("webby " + line);
}

const packageError = webby.makeDomException();
log("web class maps: " + (webby.PackageDOMException === DOMException));
log("web exception from a package: " + (packageError instanceof DOMException) + " " +
  (packageError instanceof Error) + " " + packageError.name + " " + packageError.message + " " +
  packageError.code);
log("web getter forwarded: " +
  Object.getOwnPropertyDescriptor(DOMException.prototype, "code").get.call(packageError));
log("web exception toString: " + String(packageError));
console.log(packageError);

const packageDecoder = webby.makeDecoder();
log("web decoder from a package: " + packageDecoder.encoding + " " + packageDecoder.fatal + " " +
  packageDecoder.decode(new Uint8Array([0x41, 0, 0x42, 0])));
try {
  packageDecoder.decode(new Uint8Array([0x41]));
  log("SHOULD NOT DECODE");
} catch (e) {
  log("web forwarded error: " + (e instanceof TypeError));
}
const into = new Uint8Array(2);
log("web encodeInto forwarded: " + JSON.stringify(webby.makeEncoder().encodeInto("abc", into)) +
  " " + into.join(","));
log("web app decoder in a package: " +
  webby.decodeWith(new TextDecoder(), new Uint8Array([0x6f, 0x6b])));
log("web app exception in a package: " +
  webby.describeDom(new DOMException("app message", "SyntaxError")));
try {
  webby.throwDom();
} catch (e) {
  log("web thrown across: " + (e instanceof DOMException) + " " + e.name + " " + e.code);
}

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
