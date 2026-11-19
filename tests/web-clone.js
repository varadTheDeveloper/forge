// Phase 10.4: structuredClone.
//
// The cases follow the Web Platform Tests' structured clone battery
// (html/webappapis/structured-clone/structured-clone-battery-of-tests.js and
// ...-with-transferables.js), transcribed by hand under the same names, so
// tests/wpt/coverage.json can map each one (decision D29). Cases that need
// a browser feature Forge does not have (Blob, File, ImageData, canvas,
// SharedArrayBuffer) are recorded there as not applicable. Forge's own
// checks follow. Cases transcribed from WPT are under WPT's 3-Clause BSD
// License (tests/wpt/LICENSE.md).
//
// Every failure prints a line starting with "CLONE FAIL"; the last line is
// "clone done: <checks> checks, <failures> failed".

"use strict";

let checks = 0;
let failures = 0;

function fail(name, e) {
  failures++;
  console.log(`CLONE FAIL ${name}: ${e && e.message ? e.message : String(e)}`);
}

function test(name, body) {
  checks++;
  try {
    body();
  } catch (e) {
    fail(name, e);
  }
}

function assert(condition, what) {
  if (!condition) {
    throw new Error(what);
  }
}

function same(actual, expected, what) {
  if (!Object.is(actual, expected)) {
    throw new Error(`${what}: got ${String(actual)}, expected ${String(expected)}`);
  }
}

function throwsDom(name, body, what) {
  try {
    body();
  } catch (e) {
    if (!(e instanceof DOMException) || e.name !== name) {
      throw new Error(`${what}: threw ${e && e.name} (${e && e.message}) instead of ${name}`);
    }
    return e;
  }
  throw new Error(`${what}: did not throw`);
}

function throwsType(type, body, what) {
  try {
    body();
  } catch (e) {
    if (!(e instanceof type)) {
      throw new Error(`${what}: threw ${e && e.name} instead of ${type.name}`);
    }
    return e;
  }
  throw new Error(`${what}: did not throw`);
}

// The battery's check(): clone `input` (or what the function returns) and
// compare.
function check(description, input, compare) {
  test(description, () => {
    const value = typeof input === "function" ? input() : input;
    const copy = structuredClone(value);
    compare(copy, value);
  });
}

function comparePrimitive(actual, input) {
  same(actual, input, "value");
}

function compareArray(callback) {
  return (actual, input) => {
    assert(actual instanceof Array, "instanceof Array");
    assert(actual !== input, "a new array");
    same(actual.length, input.length, "length");
    callback(actual, input);
  };
}

function compareObject(callback) {
  return (actual, input) => {
    assert(actual instanceof Object, "instanceof Object");
    assert(!(actual instanceof Array), "not an Array");
    assert(actual !== input, "a new object");
    callback(actual, input);
  };
}

function enumerateProps(compare) {
  return (actual, input) => {
    for (const x in input) {
      compare(actual[x], input[x]);
    }
  };
}

// ---------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------

check("primitive undefined", undefined, comparePrimitive);
check("primitive null", null, comparePrimitive);
check("primitive true", true, comparePrimitive);
check("primitive false", false, comparePrimitive);
check("primitive string, empty string", "", comparePrimitive);
check("primitive string, lone high surrogate", "\uD800", comparePrimitive);
check("primitive string, lone low surrogate", "\uDC00", comparePrimitive);
check("primitive string, NUL", "\u0000", comparePrimitive);
check("primitive string, astral character", "􏿽", comparePrimitive);
check("primitive number, 0.2", 0.2, comparePrimitive);
check("primitive number, 0", 0, comparePrimitive);
check("primitive number, -0", -0, comparePrimitive);
check("primitive number, NaN", NaN, comparePrimitive);
check("primitive number, Infinity", Infinity, comparePrimitive);
check("primitive number, -Infinity", -Infinity, comparePrimitive);
check("primitive number, 9007199254740992", 9007199254740992, comparePrimitive);
check("primitive number, -9007199254740992", -9007199254740992, comparePrimitive);
check("primitive number, 9007199254740994", 9007199254740994, comparePrimitive);
check("primitive number, -9007199254740994", -9007199254740994, comparePrimitive);
check("primitive BigInt, 0n", 0n, comparePrimitive);
check("primitive BigInt, -0n", -0n, comparePrimitive);
check("primitive BigInt, -9007199254740994000n", -9007199254740994000n, comparePrimitive);
check("primitive BigInt, -9007199254740994000900719925474099400090071992547409940009007199254740994000n",
      -9007199254740994000900719925474099400090071992547409940009007199254740994000n, comparePrimitive);

check("Array primitives", [undefined, null, true, false, "", "\uD800", "\uDC00", "\u0000",
                           "􏿽", 0.2, 0, -0, NaN, Infinity, -Infinity, 9007199254740992,
                           -9007199254740992, 9007199254740994, -9007199254740994, -12n, -0n, 0n],
      compareArray(enumerateProps(comparePrimitive)));
check("Object primitives", {
  "undefined": undefined, "null": null, "true": true, "false": false, "empty": "",
  "high surrogate": "\uD800", "low surrogate": "\uDC00", "nul": "\u0000", "astral": "􏿽",
  "0.2": 0.2, "0": 0, "-0": -0, "NaN": NaN, "Infinity": Infinity, "-Infinity": -Infinity,
  "9007199254740992": 9007199254740992, "-9007199254740992": -9007199254740992,
  "9007199254740994": 9007199254740994, "-9007199254740994": -9007199254740994,
}, compareObject(enumerateProps(comparePrimitive)));

// ---------------------------------------------------------------------
// Wrapper objects, Date, RegExp
// ---------------------------------------------------------------------

function compareBoolean(actual, input) {
  assert(actual instanceof Boolean, "instanceof Boolean");
  same(String(actual), String(input), "converted to primitive");
  assert(actual !== input, "a new object");
}

check("Boolean true", new Boolean(true), compareBoolean);
check("Boolean false", new Boolean(false), compareBoolean);
check("Array Boolean objects", [new Boolean(true), new Boolean(false)],
      compareArray(enumerateProps(compareBoolean)));
check("Object Boolean objects", { "true": new Boolean(true), "false": new Boolean(false) },
      compareObject(enumerateProps(compareBoolean)));

function compareObj(Type) {
  return (actual, input) => {
    assert(actual instanceof Type, `instanceof ${Type.name}`);
    same(Type(actual), Type(input), "converted to primitive");
    assert(actual !== input, "a new object");
  };
}

check("String empty string", new String(""), compareObj(String));
check("String lone high surrogate", new String("\uD800"), compareObj(String));
check("String lone low surrogate", new String("\uDC00"), compareObj(String));
check("String NUL", new String("\u0000"), compareObj(String));
check("String astral character", new String("􏿽"), compareObj(String));
check("Array String objects", [new String(""), new String("\uD800"), new String("\uDC00"),
                               new String("\u0000"), new String("􏿽")],
      compareArray(enumerateProps(compareObj(String))));
check("Object String objects", {
  "empty": new String(""), "high surrogate": new String("\uD800"), "low surrogate": new String("\uDC00"),
  "nul": new String("\u0000"), "astral": new String("􏿽"),
}, compareObject(enumerateProps(compareObj(String))));

// The names are written out so tools/wpt-coverage.py --check can find
// each one in this file.
const numberChecks = [
  ["Number 0.2", 0.2], ["Number 0", 0], ["Number -0", -0], ["Number NaN", NaN],
  ["Number Infinity", Infinity], ["Number -Infinity", -Infinity],
  ["Number 9007199254740992", 9007199254740992], ["Number -9007199254740992", -9007199254740992],
  ["Number 9007199254740994", 9007199254740994], ["Number -9007199254740994", -9007199254740994],
];
const numberInputs = numberChecks.map(([, n]) => n);
for (const [name, n] of numberChecks) {
  check(name, new Number(n), compareObj(Number));
}
check("BigInt -9007199254740994n", Object(-9007199254740994n), compareObj(BigInt));
check("Array Number objects", numberInputs.map((n) => new Number(n)),
      compareArray(enumerateProps(compareObj(Number))));
check("Object Number objects", Object.fromEntries(numberInputs.map((n) =>
  [Object.is(n, -0) ? "-0" : String(n), new Number(n)])), compareObject(enumerateProps(compareObj(Number))));

function compareDate(actual, input) {
  assert(actual instanceof Date, "instanceof Date");
  same(Number(actual), Number(input), "converted to primitive");
  assert(actual !== input, "a new object");
}

check("Date 0", new Date(0), compareDate);
check("Date -0", new Date(-0), compareDate);
check("Date -8.64e15", new Date(-8.64e15), compareDate);
check("Date 8.64e15", new Date(8.64e15), compareDate);
check("Array Date objects", [new Date(0), new Date(-0), new Date(-8.64e15), new Date(8.64e15)],
      compareArray(enumerateProps(compareDate)));
check("Object Date objects", {
  "0": new Date(0), "-0": new Date(-0), "-8.64e15": new Date(-8.64e15), "8.64e15": new Date(8.64e15),
}, compareObject(enumerateProps(compareDate)));

function compareRegExp(expectedSource) {
  return (actual, input) => {
    assert(actual instanceof RegExp, "instanceof RegExp");
    for (const flag of ["global", "ignoreCase", "multiline", "sticky", "unicode"]) {
      same(actual[flag], input[flag], flag);
    }
    same(actual.source, expectedSource, "source");
    same(actual.lastIndex, 0, "lastIndex");
    assert(actual !== input, "a new object");
  };
}

function regExpFlagsLastIndex() {
  const r = /foo/gim;
  r.lastIndex = 2;
  return r;
}

// [name, name inside an array, name inside an object, make, source]
const regExps = [
  ["RegExp flags and lastIndex", "Array RegExp object, RegExp flags and lastIndex",
   "Object RegExp object, RegExp flags and lastIndex", regExpFlagsLastIndex, "foo"],
  ["RegExp sticky flag", "Array RegExp object, RegExp sticky flag",
   "Object RegExp object, RegExp sticky flag", () => new RegExp("foo", "y"), "foo"],
  ["RegExp unicode flag", "Array RegExp object, RegExp unicode flag",
   "Object RegExp object, RegExp unicode flag", () => new RegExp("foo", "u"), "foo"],
  ["RegExp empty", "Array RegExp object, RegExp empty", "Object RegExp object, RegExp empty",
   () => new RegExp(""), "(?:)"],
  ["RegExp slash", "Array RegExp object, RegExp slash", "Object RegExp object, RegExp slash",
   () => new RegExp("/"), "\\/"],
  ["RegExp new line", "Array RegExp object, RegExp new line", "Object RegExp object, RegExp new line",
   () => new RegExp("\n"), "\\n"],
];
for (const [name, , , make, source] of regExps) {
  check(name, make, compareRegExp(source));
}
for (const [, name, , make, source] of regExps) {
  check(name, () => [make()], compareArray(enumerateProps(compareRegExp(source))));
}
for (const [, , name, make, source] of regExps) {
  check(name, () => ({ "x": make() }), compareObject(enumerateProps(compareRegExp(source))));
}

// ---------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------

function compareError(actual, input) {
  assert(actual instanceof Error, "instanceof Error");
  same(actual.constructor, input.constructor, "constructor");
  same(actual.name, input.name, "name");
  same(Object.hasOwn(actual, "message"), Object.hasOwn(input, "message"), "message existence");
  same(actual.message, input.message, "message");
  same(actual.cause, input.cause, "cause");
  same(actual.foo, undefined, "no custom property");
}

check("Empty Error object", new Error(), compareError);
for (const [name, constructor] of [["Error object", Error], ["EvalError object", EvalError],
                                   ["RangeError object", RangeError], ["ReferenceError object", ReferenceError],
                                   ["SyntaxError object", SyntaxError], ["TypeError object", TypeError],
                                   ["URIError object", URIError]]) {
  check(name, () => {
    const error = new constructor("Error message here", { cause: "my cause" });
    error.foo = "testing";
    return error;
  }, compareError);
}

// ---------------------------------------------------------------------
// Arrays and objects
// ---------------------------------------------------------------------

check("Array sparse", new Array(10), compareArray(enumerateProps(comparePrimitive)));
check("Array with non-index property", () => {
  const rv = [];
  rv.foo = "bar";
  return rv;
}, compareArray(enumerateProps(comparePrimitive)));
check("Object with index property and length", { "0": "foo", "length": 1 },
      compareObject(enumerateProps(comparePrimitive)));
check("Array with circular reference", () => {
  const rv = [];
  rv[0] = rv;
  return rv;
}, compareArray((actual) => same(actual[0], actual, "circular")));
check("Object with circular reference", () => {
  const rv = {};
  rv.x = rv;
  return rv;
}, compareObject((actual) => same(actual.x, actual, "circular")));
check("Array with identical property values", () => {
  const obj = {};
  return [obj, obj];
}, compareArray((actual) => same(actual[0], actual[1], "identical")));
check("Object with identical property values", () => {
  const obj = {};
  return { "x": obj, "y": obj };
}, compareObject((actual) => same(actual.x, actual.y, "identical")));
check("Object with property on prototype", () => {
  const Foo = function () {};
  Foo.prototype = { "foo": "bar" };
  return new Foo();
}, compareObject((actual) => assert(!("foo" in actual), "absent")));
check("Object with non-enumerable property", () => {
  const rv = {};
  Object.defineProperty(rv, "foo", { value: "bar", enumerable: false, writable: true, configurable: true });
  return rv;
}, compareObject((actual) => assert(!("foo" in actual), "absent")));
check("Object with non-writable property", () => {
  const rv = {};
  Object.defineProperty(rv, "foo", { value: "bar", enumerable: true, writable: false, configurable: true });
  return rv;
}, compareObject((actual, input) => {
  same(actual.foo, input.foo, "value");
  actual.foo += " baz";
  same(actual.foo, input.foo + " baz", "writable");
}));
check("Object with non-configurable property", () => {
  const rv = {};
  Object.defineProperty(rv, "foo", { value: "bar", enumerable: true, writable: true, configurable: false });
  return rv;
}, compareObject((actual, input) => {
  same(actual.foo, input.foo, "value");
  delete actual.foo;
  assert(!("foo" in actual), "configurable");
}));

test("Object with a getter that throws", () => {
  const exception = new Error();
  const testObject = { get testProperty() { throw exception; } };
  try {
    structuredClone(testObject);
    throw new Error("did not throw");
  } catch (e) {
    same(e, exception, "the getter's exception");
  }
});

check("ObjectPrototype must lose its exotic-ness when cloned", () => Object.prototype, (copy, original) => {
  assert(copy !== original, "a new object");
  assert(copy instanceof Object, "instanceof Object");
  const newProto = { some: "proto" };
  Object.setPrototypeOf(copy, newProto);
  same(Object.getPrototypeOf(copy), newProto, "prototype can be set");
});

// ---------------------------------------------------------------------
// ArrayBuffers and views
// ---------------------------------------------------------------------

function compareArrayBuffer(actual, input) {
  assert(actual instanceof ArrayBuffer, "instanceof ArrayBuffer");
  same(actual.byteLength, input.byteLength, "byteLength");
  same(actual.maxByteLength, input.maxByteLength, "maxByteLength");
  same(actual.resizable, input.resizable, "resizable");
}

function compareView(Type) {
  return (actual, input) => {
    assert(actual instanceof Type, `instanceof ${Type.name}`);
    same(actual.byteLength, input.byteLength, "byteLength");
    same(actual.byteOffset, input.byteOffset, "byteOffset");
    assert(actual.buffer !== input.buffer, "a new buffer");
    if (Type !== DataView) {
      same(actual.length, input.length, "length");
      for (let i = 0; i < actual.length; ++i) {
        same(actual[i], input[i], `[${i}]`);
      }
    }
  };
}

check("Resizable ArrayBuffer", () => new ArrayBuffer(16, { maxByteLength: 1024 }), compareArrayBuffer);
check("Length-tracking TypedArray", () => new Uint8Array(new ArrayBuffer(16, { maxByteLength: 1024 })),
      compareView(Uint8Array));
check("Length-tracking DataView", () => new DataView(new ArrayBuffer(16, { maxByteLength: 1024 })),
      compareView(DataView));

test("Serializing OOB TypedArray throws", () => {
  const ab = new ArrayBuffer(16, { maxByteLength: 1024 });
  const ta = new Uint8Array(ab, 8);
  ab.resize(0);
  throwsDom("DataCloneError", () => structuredClone(ta), "out of bounds");
});

test("Serializing OOB DataView throws", () => {
  const ab = new ArrayBuffer(16, { maxByteLength: 1024 });
  const dv = new DataView(ab, 8);
  ab.resize(0);
  throwsDom("DataCloneError", () => structuredClone(dv), "out of bounds");
});

// ---------------------------------------------------------------------
// Platform objects (WPT uses Response; Forge has none until Phase 11, so
// every web API object Forge has is checked instead)
// ---------------------------------------------------------------------

test("Serializing a non-serializable platform object fails", () => {
  const objects = [new Event("x"), new CustomEvent("x"), new EventTarget(), new TextEncoder(),
                   new TextDecoder(), new AbortController(), new AbortController().signal,
                   new MessageChannel(), new MessageChannel().port1, new MessageEvent("x"), navigator,
                   globalThis];
  for (const object of objects) {
    throwsDom("DataCloneError", () => structuredClone(object), Object.prototype.toString.call(object));
    throwsDom("DataCloneError", () => structuredClone({ inside: [object] }), "nested");
  }
  class MyEvent extends Event {}
  throwsDom("DataCloneError", () => structuredClone(new MyEvent("x")), "a subclass instance");
});

// ---------------------------------------------------------------------
// With transferables
// ---------------------------------------------------------------------

test("ArrayBuffer", () => {
  const buffer = new Uint8Array([1]).buffer;
  const copy = structuredClone(buffer, { transfer: [buffer] });
  same(buffer.byteLength, 0, "detached");
  same(copy.byteLength, 1, "copy");
});

let portTransferChecked = false;
test("MessagePort", () => {
  const { port1, port2 } = new MessageChannel();
  const copy = structuredClone(port2, { transfer: [port2] });
  assert(copy instanceof MessagePort && copy !== port2, "a new MessagePort");
  port1.onmessage = (e) => {
    same(e.data, "ohai", "data");
    portTransferChecked = true;
  };
  copy.postMessage("ohai");
});

test("A detached ArrayBuffer cannot be transferred", () => {
  const buffer = new ArrayBuffer();
  structuredClone(buffer, { transfer: [buffer] });
  throwsDom("DataCloneError", () => structuredClone(buffer, { transfer: [buffer] }), "detached");
});

test("A detached platform object cannot be transferred", () => {
  const { port1 } = new MessageChannel();
  structuredClone(port1, { transfer: [port1] });
  throwsDom("DataCloneError", () => structuredClone(port1, { transfer: [port1] }), "detached");
});

test("Transferring a non-transferable platform object fails", () => {
  for (const object of [new Event("x"), new TextEncoder(), new AbortController().signal]) {
    throwsDom("DataCloneError", () => structuredClone(object, { transfer: [object] }),
              Object.prototype.toString.call(object));
  }
});

test("An object whose interface is deleted from the global object must still be received", () => {
  const { port1 } = new MessageChannel();
  const messagePortInterface = globalThis.MessagePort;
  delete globalThis.MessagePort;
  try {
    const transfer = structuredClone(port1, { transfer: [port1] });
    assert(transfer instanceof messagePortInterface, "a MessagePort");
  } finally {
    globalThis.MessagePort = messagePortInterface;
  }
});

test("Resizable ArrayBuffer is transferable", () => {
  const buffer = new ArrayBuffer(16, { maxByteLength: 1024 });
  const copy = structuredClone(buffer, { transfer: [buffer] });
  same(buffer.byteLength, 0, "detached");
  same(copy.byteLength, 16, "byteLength");
  same(copy.maxByteLength, 1024, "maxByteLength");
  assert(copy.resizable, "resizable");
});

test("Length-tracking TypedArray is transferable", () => {
  const ab = new ArrayBuffer(16, { maxByteLength: 1024 });
  const copy = structuredClone(new Uint8Array(ab), { transfer: [ab] });
  same(ab.byteLength, 0, "detached");
  same(copy.buffer.byteLength, 16, "byteLength");
  copy.buffer.resize(32);
  same(copy.byteLength, 32, "tracks the length");
});

test("Length-tracking DataView is transferable", () => {
  const ab = new ArrayBuffer(16, { maxByteLength: 1024 });
  const copy = structuredClone(new DataView(ab), { transfer: [ab] });
  same(ab.byteLength, 0, "detached");
  copy.buffer.resize(32);
  same(copy.byteLength, 32, "tracks the length");
});

test("Transferring OOB TypedArray throws", () => {
  const ab = new ArrayBuffer(16, { maxByteLength: 1024 });
  const ta = new Uint8Array(ab, 8);
  ab.resize(0);
  throwsDom("DataCloneError", () => structuredClone(ta, { transfer: [ab] }), "out of bounds");
});

test("Transferring OOB DataView throws", () => {
  const ab = new ArrayBuffer(16, { maxByteLength: 1024 });
  const dv = new DataView(ab, 8);
  ab.resize(0);
  throwsDom("DataCloneError", () => structuredClone(dv, { transfer: [ab] }), "out of bounds");
});

// ---------------------------------------------------------------------
// Forge's own checks
// ---------------------------------------------------------------------

test("structuredClone is an enumerable operation of the global object", () => {
  const d = Object.getOwnPropertyDescriptor(globalThis, "structuredClone");
  assert(d && typeof d.value === "function", "exists");
  assert(d.writable && d.enumerable && d.configurable, "attributes");
  same(structuredClone.length, 1, "length");
  same(structuredClone.name, "structuredClone", "name");
});

test("argument errors", () => {
  throwsType(TypeError, () => structuredClone(), "no argument");
  throwsType(TypeError, () => structuredClone(1, 5), "options not an object");
  throwsType(TypeError, () => structuredClone(1, { transfer: 5 }), "transfer not a sequence");
  throwsType(TypeError, () => structuredClone(1, { transfer: {} }), "transfer not iterable");
  throwsType(TypeError, () => structuredClone(1, { transfer: [1] }), "transfer item not an object");
  same(structuredClone(1, null), 1, "null options");
  same(structuredClone(1, { transfer: undefined }), 1, "undefined transfer");
  const buffer = new ArrayBuffer(4);
  throwsDom("DataCloneError", () => structuredClone(1, { transfer: [buffer, buffer] }), "twice");
  same(buffer.byteLength, 4, "nothing detached after a failure");
  throwsDom("DataCloneError", () => structuredClone(1, { transfer: [{}] }), "a plain object");
});

test("values that cannot be cloned", () => {
  for (const value of [() => {}, Symbol("s"), { f() {} }, [Symbol.iterator], new WeakMap(),
                       new WeakSet(), Promise.resolve(), new Proxy({}, {})]) {
    throwsDom("DataCloneError", () => structuredClone(value), String(typeof value));
  }
});

test("capability objects cannot be cloned", () => {
  // Forge's namespaces hold functions, which never clone: no capability
  // can be copied out through structuredClone.
  for (const value of [Forge, console, Forge.env]) {
    throwsDom("DataCloneError", () => structuredClone(value), "a namespace");
  }
});

test("Map, Set, typed arrays and shared references", () => {
  const shared = { n: 1 };
  const map = new Map([[shared, "v"], ["k", shared]]);
  const set = new Set([shared, 2]);
  const bytes = new Uint16Array([1, 2, 3]);
  const copy = structuredClone({ map, set, bytes, view: new DataView(bytes.buffer, 2), shared });
  assert(copy.map instanceof Map && copy.set instanceof Set, "types");
  const key = [...copy.map.keys()][0];
  same(key, copy.shared, "a Map key keeps its identity");
  same(copy.map.get("k"), copy.shared, "a Map value too");
  assert(copy.set.has(copy.shared), "a Set member too");
  same(copy.bytes[2], 3, "typed array contents");
  same(copy.view.buffer, copy.bytes.buffer, "views share their buffer");
});

test("getters run once each, in property order", () => {
  const order = [];
  const value = {
    get a() { order.push("a"); return 1; },
    get b() { order.push("b"); return 2; },
  };
  const copy = structuredClone(value);
  same(order.join(), "a,b", "order");
  assert(Object.getOwnPropertyDescriptor(copy, "a").value === 1, "a data property in the copy");
});

test("DOMException clones as an Error (documented deviation)", () => {
  // HTML says a DOMException is serializable; Forge's DOMException is an
  // engine Error object, so its clone is a plain Error (WinterTC.md).
  const copy = structuredClone(new DOMException("m", "AbortError"));
  assert(copy instanceof Error, "an Error");
  assert(!(copy instanceof DOMException), "not a DOMException yet");
});

test("changing the built-ins does not change structuredClone", () => {
  const saved = Array.prototype[Symbol.iterator];
  let called = false;
  Object.defineProperty(Array.prototype, "0", {
    set() { called = true; }, get() { return undefined; }, configurable: true,
  });
  try {
    const { port1 } = new MessageChannel();
    const buffer = new ArrayBuffer(2);
    structuredClone({ port1, buffer }, { transfer: new Set([port1, buffer]) });
    same(called, false, "no setter on Array.prototype ran");
  } finally {
    delete Array.prototype[0];
    Array.prototype[Symbol.iterator] = saved;
  }
});

setTimeout(() => {
  checks++;
  if (!portTransferChecked) {
    fail("MessagePort (message received)", new Error("the message never arrived"));
  }
  console.log(`clone done: ${checks} checks, ${failures} failed`);
  if (failures > 0) {
    Forge.exitCode = 1;
  }
}, 50);
