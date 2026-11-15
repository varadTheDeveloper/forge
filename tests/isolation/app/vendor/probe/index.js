// Phase 9.3: a permission-free package, so it runs in the shared "pure"
// compartment. Its top level records what it can and cannot do there; its
// functions are called by the program with the program's own values.

export const checks = [];
const check = (text) => checks.push(text);

function attempt(action) {
  try {
    return "allowed: " + action();
  } catch (e) {
    return (e && e.code) || (e && e.name) || String(e);
  }
}

// No authority at all: no fs, no Forge namespace, no print.
check("globals: " + [typeof fs, typeof Forge, typeof print].join(" "));
check("safe globals: " + [typeof console.log, typeof setTimeout, typeof queueMicrotask].join(" "));

// Strings never become code.
check("eval: " + attempt(() => eval("1 + 1")));
check("indirect eval: " + attempt(() => (0, eval)("1 + 1")));
check("new Function: " + attempt(() => new Function("return 1")()));

// Frozen intrinsics and a frozen global object.
check("frozen: " + [
  Object.isFrozen(Object.prototype),
  Object.isFrozen(Array.prototype),
  Object.isFrozen(Function.prototype),
  Object.isFrozen(Object.getPrototypeOf([][Symbol.iterator]())),
  Object.isFrozen(globalThis),
  Object.isFrozen(console),
].join(" "));
// The hidden intrinsics, which no property path from the global reaches.
const proto = Object.getPrototypeOf;
check("hidden frozen: " + [
  proto(""[Symbol.iterator]()),
  proto(new Map().entries()),
  proto(new Set().values()),
  proto(/a/g[Symbol.matchAll]("a")),
  proto(function* () {}),
  proto(async function () {}),
  proto(async function* () {}),
  proto(function* () {}).prototype,
  proto([1].values().map((x) => x)),
].map((o) => Object.isFrozen(o)).join(" "));
check("pollute Object.prototype: " + attempt(() => { Object.prototype.polluted = 1; return 1; }));
check("then on Promise.prototype: " + attempt(() => { Promise.prototype.then = () => {}; return 1; }));
check("global variable: " + attempt(() => { globalThis.leaked = 1; return 1; }));
check("replace console.log: " + attempt(() => { console.log = () => {}; return 1; }));

// The "override mistake" is handled: these assignments still work.
function MyError(message) {
  this.message = message;
  this.name = "MyError";
}
MyError.prototype = Object.create(Error.prototype);
MyError.prototype.constructor = MyError;
const custom = {};
custom.toString = () => "custom-text";  // inherited from a frozen prototype
const own = new MyError("mine");
check("override: " + own.name + " " + own.message + " " + String(custom) + " " +
  (own instanceof Error));

// The app's top-level bindings are not visible here (CVE-2025-32792).
check("app lexical: " + typeof appSecret);

// No realm-making API is reachable.
check("new realms: " + [typeof ShadowRealm, typeof Forge].join(" "));

export const metaOk = import.meta.url.endsWith("/vendor/probe/index.js") &&
  typeof import.meta.resolve === "function";

export const sample = { id: 7, list: [1, 2] };

export function identity(value) {
  return value;
}

export function pollute(obj) {
  return attempt(() => {
    Object.getPrototypeOf(obj).polluted = "yes";
    return "wrote";
  });
}

export function protoIsOwn(obj, arr) {
  return Object.getPrototypeOf(obj) === Object.prototype && arr instanceof Array &&
    Array.isArray(arr);
}

export function reachFunction(obj) {
  return attempt(() => typeof obj.constructor.constructor("return globalThis")());
}

export function readProperty(holder, name) {
  return attempt(() => typeof holder[name]);
}

export function callBack(fn) {
  return fn();
}

export function fail() {
  throw new TypeError("probe failed");
}

export function regexpLastMatch() {
  return RegExp.lastMatch;
}

export function clear(id) {
  clearTimeout(id);
  return "cleared";
}

export function later(value) {
  return new Promise((resolve) => setTimeout(() => resolve(value), 5));
}

export function micro(fn) {
  return new Promise((resolve) => queueMicrotask(() => resolve(fn())));
}

export function crossMicro(fn) {
  queueMicrotask(fn);  // a wrapper of the program's function
}

export async function loadHelper() {
  const helper = await import("@iso/helper");
  return helper.value;
}

// LavaMoat's first escape (CVE-2023-39532) hid import( behind a spread.
// Forge never scans source: the load hook sees every import.
export async function spreadImport() {
  try {
    const ns = { ...(await import("fs")) };
    return "loaded " + Object.keys(ns).length;
  } catch (e) {
    return e.code || e.name;
  }
}
