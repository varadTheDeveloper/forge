// Phase 8.4 (rule R24): a deterministic fuzzer for every native function
// Forge exposes to JavaScript.
//
//   forge --allow-read=<scratch> --allow-write=<scratch> tests/fuzz-natives.js <scratch> [seed] [calls]
//
// Run it with the current folder set to <scratch> (the runtime test runner
// does), so relative paths stay inside the grant. It calls each native
// with adversarial values and checks, after every call:
//
//   1. Forge did not crash and the call either returned or threw an Error
//      (never a bare string or other odd value).
//   2. A Promise-returning fs function never threw synchronously.
//   3. Any `code` on an error is one of Forge's error codes.
//   4. Natives that promise never to run script code (everything except
//      print and the timer functions, see CallSites.md) did not call a
//      getter, a toString/valueOf, a Symbol.toPrimitive method or a Proxy
//      trap of any argument.
//
// The same seed gives the same calls, so a failure can be replayed by
// passing the seed it printed. Forge.exit is the one native left out
// (it ends the program by design). Callbacks given to timers,
// queueMicrotask and signal listeners never throw, so no uncaught
// exception can end the run early.
//
// Prints "fuzz done" at the end; any problem is printed as a line starting
// with "FUZZ VIOLATION" and makes the exit code 1.

"use strict";

const scratch = Forge.args[0];
const seed = Forge.args[1] !== undefined ? Number(Forge.args[1]) >>> 0 : 0x5eed2026;
const totalCalls = Forge.args[2] !== undefined ? Number(Forge.args[2]) : 4000;

if (typeof scratch !== "string") {
  throw new Error("usage: fuzz-natives.js <scratch folder> [seed] [calls]");
}

// ------------------------------------------------------------ helpers ----

// mulberry32: small, fast and deterministic.
let state = seed;
function random() {
  state = (state + 0x6d2b79f5) >>> 0;
  let t = state;
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
const pick = (list) => list[Math.floor(random() * list.length)];
const chance = (p) => random() < p;

const out = (text) => Forge.stdout.write(text + "\n");
const violations = [];
function violation(text) {
  if (violations.length < 50) {
    violations.push(text);
  }
}

// Every time script code runs because a native touched an argument, this
// counter goes up.
let tripped = 0;
let callbackRuns = 0;
const SENTINEL = { fuzzSentinel: true };
const countAndReturn = (value) => () => { tripped++; return value; };
const countAndThrow = () => { tripped++; throw SENTINEL; };

// A Proxy handler that counts every trap the engine looks up.
const countingHandler = new Proxy({}, {
  get(_target, trap) {
    tripped++;
    return Reflect[trap];
  },
});
const counting = (target) => new Proxy(target, countingHandler);

const ERROR_CODES = new Set([
  "Unknown", "InvalidArgument", "InvalidOperation", "NotSupported", "NotImplemented",
  "NotFound", "AlreadyExists", "PermissionDenied", "Busy", "Timeout", "Cancelled",
  "EndOfFile", "IOError", "OutOfMemory", "BufferTooSmall", "Overflow", "Underflow",
  "InvalidData", "ParseError", "PlatformError",
]);

// ------------------------------------------------------------- values ----

const safeFunctions = [
  () => { callbackRuns++; },
  function named() { callbackRuns++; },
  async function asyncSafe() { callbackRuns++; },
  function* generator() { callbackRuns++; },
];
safeFunctions.push(safeFunctions[1].bind(null, 1));
safeFunctions.push(counting(function proxied() { callbackRuns++; }));
const SAFE = new Set(safeFunctions);

function detachedBuffer() {
  const buffer = new ArrayBuffer(8);
  if (typeof buffer.transfer === "function") {
    buffer.transfer();
  }
  return buffer;
}

function detachedView() {
  const view = new Uint8Array(new ArrayBuffer(8));
  if (typeof view.buffer.transfer === "function") {
    view.buffer.transfer();
  }
  return view;
}

function lengthTrackingView() {
  try {
    const buffer = new ArrayBuffer(4, { maxByteLength: 64 });
    const view = new Uint8Array(buffer);
    buffer.resize(16);
    return view;
  } catch {
    return new Uint8Array(4);
  }
}

function cyclic() {
  const a = { name: "a" };
  a.self = a;
  a.list = [a, { back: a }];
  return a;
}

function deepArray(depth) {
  let v = [];
  for (let i = 0; i < depth; i++) {
    v = [v];
  }
  return v;
}

function accessorError() {
  const e = new Error("plain");
  Object.defineProperty(e, "message", { get: countAndThrow, configurable: true });
  Object.defineProperty(e, "stack", { get: countAndThrow, configurable: true });
  Object.defineProperty(e, "name", { get: countAndThrow, configurable: true });
  return e;
}

class TrapMap extends Map {
  forEach() { tripped++; }
  entries() { tripped++; return [][Symbol.iterator](); }
  get size() { tripped++; return 99; }
}

class WithGetter {
  get secret() { tripped++; return "x"; }
}

function trappedArray() {
  const a = [1, 2, 3];
  Object.defineProperty(a, 1, { get: countAndThrow, enumerable: true });
  return a;
}

function trappedTypedArray() {
  const u = new Uint8Array(4);
  Object.defineProperty(u, "length", { get: countAndReturn(1e9) });
  Object.defineProperty(u, "byteLength", { get: countAndReturn(1e9) });
  return u;
}

function fakeBytes() {
  return Object.defineProperties({}, {
    length: { get: countAndReturn(3), enumerable: true },
    0: { get: countAndReturn(65), enumerable: true },
    buffer: { get: countAndReturn(new ArrayBuffer(3)) },
  });
}

function revokedProxy() {
  const r = Proxy.revocable({}, {});
  r.revoke();
  return r.proxy;
}

const rejected = Promise.reject(new Error("pool rejection"));
rejected.catch(() => {});

const tripwires = [
  { toString: countAndThrow },
  { toString: countAndReturn("forge-fuzz-from-toString") },
  { valueOf: countAndThrow, toString: countAndThrow },
  { [Symbol.toPrimitive]: countAndReturn("C:\\Windows\\win.ini") },
  { [Symbol.toPrimitive]: countAndReturn(5) },
  Object.defineProperty({}, "recursive", { get: countAndReturn(true), enumerable: true }),
  Object.defineProperty({}, "withFileTypes", { get: countAndReturn(true), enumerable: true }),
  Object.defineProperty({}, Symbol.toStringTag, { get: countAndThrow }),
  Object.defineProperty({}, "constructor", { get: countAndThrow }),
  counting({}),
  counting({ recursive: true, withFileTypes: true }),
  counting([1, 2, 3]),
  counting(new Uint8Array(4)),
  counting(new Map([[1, 2]])),
  counting(new Error("proxied error")),
  accessorError(),
  new TrapMap([[1, { a: 1 }]]),
  new WithGetter(),
  trappedArray(),
  trappedTypedArray(),
  fakeBytes(),
  (() => { const d = new Date(0); d.toString = countAndThrow; d.toISOString = countAndThrow; return d; })(),
  (() => { const r = /x/g; Object.defineProperty(r, "source", { get: countAndThrow }); return r; })(),
];

const smallStrings = [
  "", "a", " ", ".", "..", "\0", "a\0b", "\uD800", "\uDFFF\uD800", "caf\u00e9 \u{1F525}",
  "%s %d %i %f %o %O %c %%", "%", "%%%", "%s", "\n\r\t", "\u202Eevil", "C:", "C:\\",
  "con", "NUL.txt", "a:b", "x::$DATA", "trailing.", "trailing ", "*", "?", "<>|\"",
  "\\\\?\\C:\\Windows", "\\\\.\\PhysicalDrive0", "\\\\127.0.0.1\\c$\\x", "\\??\\C:\\x",
  "GLOBALROOT", "\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1", "../outside", "..\\..\\..",
  "/", "\\", "//", "C:file", ".git", "forge.json", "FORGE.LOCK", "GIT~1",
];

const bigStrings = [
  "x".repeat(300), "a\\".repeat(200), "\u00e9".repeat(5000), "y".repeat(40000),
];

const numbers = [
  0, -0, 1, -1, 0.5, NaN, Infinity, -Infinity, 255, 256, 2 ** 31 - 1, 2 ** 31,
  2 ** 32, 2 ** 53, Number.MAX_VALUE, Number.MIN_VALUE,
];

const others = [
  undefined, null, true, false, 0n, -1n, 2n ** 64n, Symbol(), Symbol("desc"),
  Symbol.iterator, [], [1, [2, [3]]], {}, Object.create(null), cyclic(), deepArray(3000),
  new Error("an error"), new TypeError("type"), new AggregateError([new Error("inner")], "agg"),
  /re/g, new Date(0), new Date(NaN), new Map([[1, { a: 1 }]]), new Set([1, "two"]),
  new WeakMap(), new WeakRef({}), new DataView(new ArrayBuffer(4)), new Float64Array([NaN, -0]),
  new BigInt64Array(2), new Uint8Array(0), new Uint8Array([104, 105]), new ArrayBuffer(8),
  detachedBuffer(), detachedView(), lengthTrackingView(), new Uint16Array(3),
  Promise.resolve(1), rejected, revokedProxy(), class K {}, Math, JSON, Reflect,
  globalThis, Forge, fs, console, Forge.args,
  ...safeFunctions,
];

const bigValues = [new Uint8Array(1 << 20), new ArrayBuffer(1 << 18), ...bigStrings];

const outputPool = [...smallStrings, ...numbers, ...others, ...tripwires];
const fullPool = [...outputPool, ...bigValues];

// Paths relative to the scratch folder (the current folder).
const pathPool = [
  "f0", "f1", "f2.txt", "d0", "d0/sub", "d0/sub/f.txt", "d1", "d1/x", ".", "./f0",
  "d0/../f1", "D0/SUB", "f0/", "new-folder/a/b",
  scratch, scratch + "\\f0", scratch + "/d0/sub",
];

const optionPool = [
  undefined, {}, { recursive: true }, { recursive: false }, { withFileTypes: true },
  { recursive: 1 }, { recursive: "yes" }, Object.create({ recursive: true }),
  Object.assign(Object.create(null), { recursive: true }), null, 1, "utf8",
  ...tripwires.slice(5, 11),
];

function anyValue(output) {
  return pick(output ? outputPool : fullPool);
}

function pathValue() {
  if (chance(0.6)) {
    return pick(pathPool);
  }
  return chance(0.6) ? pick(smallStrings) : anyValue(false);
}

function callbackValue() {
  const v = chance(0.6) ? pick(safeFunctions) : anyValue(true);
  // Never hand a throwing callable to something that will call it later.
  return typeof v === "function" && !SAFE.has(v) ? pick(safeFunctions) : v;
}

// ------------------------------------------------------------ natives ----

// Collected by walking the objects, so a native added later is fuzzed
// without changing this file.
const natives = [];

function collect(object, prefix) {
  for (const key of Object.getOwnPropertyNames(object)) {
    const d = Object.getOwnPropertyDescriptor(object, key);
    const name = prefix + key;

    if (name === "Forge.exit") {
      continue;
    }

    if (typeof d.value === "function") {
      natives.push({ name, fn: d.value, self: object });
    } else if (prefix === "Forge." && d.value && typeof d.value === "object" &&
               !Array.isArray(d.value)) {
      collect(d.value, name + ".");
    }
  }
}

collect(fs, "fs.");
collect(console, "console.");
collect(Forge, "Forge.");
for (const g of ["setTimeout", "setInterval", "clearTimeout", "clearInterval",
                 "queueMicrotask", "print"]) {
  natives.push({ name: g, fn: globalThis[g], self: globalThis });
}
natives.push({ name: "Forge.exitCode=", fn(v) { Forge.exitCode = v; }, self: undefined });

// These may run script code by design (CallSites.md): print converts with
// ToString and the timer functions convert the delay or id with ToNumber.
const MAY_RUN_SCRIPT = new Set(["print", "setTimeout", "setInterval", "clearTimeout",
                                "clearInterval"]);
const OUTPUT = (name) => name.startsWith("console.") || name === "print" ||
                         name.startsWith("Forge.stdout") || name.startsWith("Forge.stderr");
const TWO_PATHS = new Set(["renameSync", "rename", "copyFileSync", "copyFile"]);
const WITH_DATA = new Set(["writeFileSync", "writeFile", "appendFileSync", "appendFile"]);
const WITH_OPTIONS = new Set(["readdirSync", "readdir", "mkdirSync", "mkdir", "rmSync", "rm"]);
const WITH_ENCODING = new Set(["readFileSync", "readFile"]);

function argumentsFor(native) {
  const name = native.name;
  const count = Math.floor(random() * 5);

  if (name.startsWith("fs.")) {
    const short = name.slice(3);
    const args = [pathValue()];

    if (TWO_PATHS.has(short)) {
      args.push(pathValue());
    } else if (WITH_DATA.has(short)) {
      args.push(anyValue(false));
    } else if (WITH_OPTIONS.has(short)) {
      args.push(pick(optionPool));
    } else if (WITH_ENCODING.has(short)) {
      args.push(chance(0.5) ? "utf8" : anyValue(false));
    }

    while (chance(0.15)) {
      args.push(anyValue(false));
    }

    return chance(0.05) ? args.slice(0, count % (args.length + 1)) : args;
  }

  if (name === "setTimeout" || name === "setInterval" || name === "queueMicrotask" ||
      name === "Forge.addSignalListener" || name === "Forge.removeSignalListener") {
    const args = name.includes("SignalListener")
      ? [chance(0.7) ? pick(["SIGINT", "SIGBREAK"]) : anyValue(true), callbackValue()]
      : [callbackValue()];

    for (let i = 0; i < count; i++) {
      args.push(chance(0.5) ? pick(numbers) : anyValue(true));
    }

    return args;
  }

  const args = [];
  for (let i = 0; i < count; i++) {
    args.push(anyValue(OUTPUT(name)));
  }
  return args;
}

// --------------------------------------------------------------- run ----

const timers = [];
const promises = [];
const calls = new Map();
let throws = 0;

function checkThrown(native, value, args) {
  if (value === SENTINEL) {
    if (!MAY_RUN_SCRIPT.has(native.name)) {
      violation(`${native.name}: script code threw through it`);
    }
    return;
  }

  if (!(value instanceof Error)) {
    violation(`${native.name}: threw a non-Error ${typeof value}: ${String(value).slice(0, 80)}`);
    return;
  }

  const own = Object.getOwnPropertyDescriptor(value, "code");
  if (own && !ERROR_CODES.has(own.value)) {
    violation(`${native.name}: unknown error code ${String(own.value)}`);
  }
}

function describe(args) {
  return args.map((a) => {
    try {
      if (typeof a === "string") return JSON.stringify(a.length > 40 ? a.slice(0, 40) + "..." : a);
      if (typeof a === "function") return "function";
      if (typeof a === "symbol" || typeof a === "bigint") return typeof a;
      return Object.prototype.toString.call(a);
    } catch {
      return "?";
    }
  }).join(", ");
}

out(`fuzz: seed ${seed}, ${totalCalls} calls over ${natives.length} natives`);

if (natives.length < 57) {
  violation(`only ${natives.length} natives were found; expected at least 57`);
}

for (let i = 0; i < totalCalls; i++) {
  const native = pick(natives);
  const args = argumentsFor(native);
  const self = chance(0.1) ? anyValue(true) : native.self;
  let result;
  let threw = false;
  let thrown;

  calls.set(native.name, (calls.get(native.name) || 0) + 1);

  // Nothing between these two reads may touch an argument.
  const before = tripped;
  try {
    result = Reflect.apply(native.fn, self, args);
  } catch (e) {
    threw = true;
    thrown = e;
  }
  const ranScript = tripped !== before;

  if (ranScript && !MAY_RUN_SCRIPT.has(native.name)) {
    violation(`${native.name}: ran script code of an argument (${describe(args)})`);
  }

  if (threw) {
    throws++;
    checkThrown(native, thrown, args);
    if (native.name.startsWith("fs.") && !native.name.endsWith("Sync")) {
      violation(`${native.name}: threw synchronously instead of rejecting (${describe(args)})`);
    }
  }

  if (native.name === "setTimeout" || native.name === "setInterval") {
    if (typeof result === "number") {
      timers.push(result);
    }
  } else if (native.name.startsWith("fs.") && !native.name.endsWith("Sync")) {
    if (!(result instanceof Promise)) {
      violation(`${native.name}: did not return a Promise`);
    } else {
      const name = native.name;
      promises.push(result.then(
        () => {},
        (e) => checkThrown({ name }, e, args)));
    }
  }

  // Keep the scratch tree from disappearing for long.
  if (i % 200 === 0) {
    try { fs.mkdirSync("d0/sub", { recursive: true }); } catch {}
    try { fs.writeFileSync("f0", "seed"); } catch {}
  }
}

// Stop everything the run started, then wait for the fs work to finish.
for (const id of timers) {
  clearTimeout(id);
}

for (const fn of safeFunctions) {
  Forge.removeSignalListener("SIGINT", fn);
  Forge.removeSignalListener("SIGBREAK", fn);
}

for (let i = 0; i < 256; i++) {
  console.groupEnd();
}

Forge.exitCode = 0;

(async () => {
  await Promise.allSettled(promises);

  // Fuzzing must not have widened anything: the folder above the scratch
  // folder is still not readable.
  try {
    fs.readdirSync("..");
    violation("the folder above the scratch folder became readable");
  } catch (e) {
    if (e.code !== "PermissionDenied") {
      violation(`reading .. failed with ${e.code} instead of PermissionDenied`);
    }
  }

  const covered = natives.filter((n) => calls.has(n.name)).length;
  out(`fuzz: ${throws} throws, ${promises.length} promises, ${covered}/${natives.length} natives called`);

  if (covered < natives.length && totalCalls >= 2000) {
    const missed = natives.filter((n) => !calls.has(n.name)).map((n) => n.name);
    violation(`never called: ${missed.join(", ")}`);
  }

  for (const v of violations) {
    out("FUZZ VIOLATION " + v);
  }

  if (violations.length > 0) {
    Forge.exitCode = 1;
  }

  out("fuzz done");
})().catch((e) => {
  out("FUZZ VIOLATION the fuzzer itself failed: " + (e && e.message));
  Forge.exitCode = 1;
});
