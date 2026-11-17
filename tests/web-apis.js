// Phase 10.1: DOMException, TextEncoder, TextDecoder, atob and btoa.
//
// The checks follow the Web Platform Tests for these APIs (webidl/
// ecmascript-binding/es-exceptions, encoding/, html/webappapis/atob),
// transcribed by hand: Forge does not run the WPT files themselves yet
// (that is the WPT runner, PHASE10_DESIGN.md). Every failure prints a line
// starting with "WEB FAIL" and makes the exit code 1; the last line is
// "web done: <checks> checks".

"use strict";

let checks = 0;
let failures = 0;

function fail(name, why) {
  failures++;
  console.log(`WEB FAIL ${name}: ${why}`);
}

function test(name, body) {
  checks++;
  try {
    body();
  } catch (e) {
    fail(name, e && e.stack ? e.stack.split("\n")[0] + " / " + String(e) : String(e));
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

function throwsDom(name, body, what) {
  const e = throwsType(DOMException, body, what);
  same(e.name, name, what + " (name)");
  return e;
}

function bytes(list) {
  return Uint8Array.from(list);
}

function sameBytes(actual, expected, what) {
  same(actual.length, expected.length, what + " (length)");
  for (let i = 0; i < expected.length; i++) {
    same(actual[i], expected[i], `${what} [${i}]`);
  }
}

function hex(s) {
  let out = "";
  for (let i = 0; i < s.length; i++) {
    out += s.charCodeAt(i).toString(16).padStart(4, "0") + " ";
  }
  return out.trim();
}

function sameText(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(`${what}: got [${hex(actual)}], expected [${hex(expected)}]`);
  }
}

// ---------------------------------------------------------------------
// Global properties (WebIDL)
// ---------------------------------------------------------------------

for (const name of ["DOMException", "TextEncoder", "TextDecoder"]) {
  test(`${name} global property`, () => {
    const d = Object.getOwnPropertyDescriptor(globalThis, name);
    assert(d && typeof d.value === "function", "exists");
    assert(d.writable && !d.enumerable && d.configurable, "writable, not enumerable, configurable");
    const p = Object.getOwnPropertyDescriptor(d.value, "prototype");
    assert(!p.writable && !p.enumerable && !p.configurable, "prototype is fixed");
    const c = Object.getOwnPropertyDescriptor(d.value.prototype, "constructor");
    assert(c.writable && !c.enumerable && c.configurable && c.value === d.value, "constructor");
    same(Object.prototype.toString.call(d.value.prototype), `[object ${name}]`, "toStringTag");
    same(d.value.length, 0, "length");
    same(d.value.name, name, "name");
  });
}

for (const name of ["atob", "btoa"]) {
  test(`${name} global property`, () => {
    const d = Object.getOwnPropertyDescriptor(globalThis, name);
    assert(d && typeof d.value === "function", "exists");
    assert(d.writable && d.enumerable && d.configurable, "writable, enumerable, configurable");
    same(d.value.length, 1, "length");
    same(d.value.name, name, "name");
    assert(!("prototype" in d.value), "no prototype");
    throwsType(TypeError, () => new d.value("YQ=="), "not a constructor");
    throwsType(TypeError, () => d.value(), "needs one argument");
  });
}

// ---------------------------------------------------------------------
// DOMException
// ---------------------------------------------------------------------

test("DOMException: cannot construct without new", () => {
  throwsType(TypeError, () => DOMException(), "call");
});

test("DOMException: prototype chain", () => {
  same(Object.getPrototypeOf(DOMException.prototype), Error.prototype, "prototype side");
  same(Object.getPrototypeOf(DOMException), Function.prototype, "class side");
});

for (const member of ["name", "message", "code"]) {
  test(`DOMException: ${member} is an inherited getter`, () => {
    const e = new DOMException("message", "name");
    assert(!Object.prototype.hasOwnProperty.call(e, member), "not own");
    const d = Object.getOwnPropertyDescriptor(DOMException.prototype, member);
    assert(typeof d.get === "function" && d.set === undefined, "getter only");
    assert(d.enumerable && d.configurable, "enumerable and configurable");
    throwsType(TypeError, () => d.get.call({}), "brand check");
    throwsType(TypeError, () => d.get.call(new Error("x")), "brand check on an Error");
  });
}

test("DOMException: defaults", () => {
  const e = new DOMException();
  same(e.name, "Error", "name");
  same(e.message, "", "message");
  same(e.code, 0, "code");
  assert(!Object.prototype.hasOwnProperty.call(e, "message"), "no own message");
  same(new DOMException(null).message, "null", "null message");
  same(new DOMException(undefined).message, "", "undefined message");
  same(new DOMException("bar", undefined).name, "Error", "undefined name");
});

test("DOMException: names and legacy codes", () => {
  const table = [
    ["IndexSizeError", 1], ["HierarchyRequestError", 3], ["WrongDocumentError", 4],
    ["InvalidCharacterError", 5], ["NoModificationAllowedError", 7], ["NotFoundError", 8],
    ["NotSupportedError", 9], ["InUseAttributeError", 10], ["InvalidStateError", 11],
    ["SyntaxError", 12], ["InvalidModificationError", 13], ["NamespaceError", 14],
    ["InvalidAccessError", 15], ["TypeMismatchError", 17], ["SecurityError", 18],
    ["NetworkError", 19], ["AbortError", 20], ["URLMismatchError", 21],
    ["QuotaExceededError", 22], ["TimeoutError", 23], ["InvalidNodeTypeError", 24],
    ["DataCloneError", 25], ["DOMStringSizeError", 0], ["NoDataAllowedError", 0],
    ["ValidationError", 0], ["EncodingError", 0], ["NotReadableError", 0],
    ["UnknownError", 0], ["ConstraintError", 0], ["DataError", 0],
    ["TransactionInactiveError", 0], ["ReadOnlyError", 0], ["VersionError", 0],
    ["OperationError", 0], ["NotAllowedError", 0], ["foo", 0], ["__proto__", 0],
    ["constructor", 0], ["toString", 0],
  ];
  for (const [name, code] of table) {
    const e = new DOMException("msg", name);
    same(e.name, name, name);
    same(e.message, "msg", name + " message");
    same(e.code, code, name + " code");
  }
});

test("DOMException: legacy constants", () => {
  const names = [
    "INDEX_SIZE_ERR", "DOMSTRING_SIZE_ERR", "HIERARCHY_REQUEST_ERR", "WRONG_DOCUMENT_ERR",
    "INVALID_CHARACTER_ERR", "NO_DATA_ALLOWED_ERR", "NO_MODIFICATION_ALLOWED_ERR",
    "NOT_FOUND_ERR", "NOT_SUPPORTED_ERR", "INUSE_ATTRIBUTE_ERR", "INVALID_STATE_ERR",
    "SYNTAX_ERR", "INVALID_MODIFICATION_ERR", "NAMESPACE_ERR", "INVALID_ACCESS_ERR",
    "VALIDATION_ERR", "TYPE_MISMATCH_ERR", "SECURITY_ERR", "NETWORK_ERR", "ABORT_ERR",
    "URL_MISMATCH_ERR", "QUOTA_EXCEEDED_ERR", "TIMEOUT_ERR", "INVALID_NODE_TYPE_ERR",
    "DATA_CLONE_ERR",
  ];
  names.forEach((name, i) => {
    for (const object of [DOMException, DOMException.prototype]) {
      const d = Object.getOwnPropertyDescriptor(object, name);
      assert(d, name + " exists");
      same(d.value, i + 1, name);
      assert(!d.writable && d.enumerable && !d.configurable, name + " attributes");
    }
  });
});

test("DOMException: code ignores a shadowing name", () => {
  const e = new DOMException("message", "InvalidCharacterError");
  Object.defineProperty(e, "name", { value: "WrongDocumentError" });
  same(e.code, 5, "code");
});

test("DOMException: toString and toStringTag", () => {
  const e = new DOMException("message", "name");
  same(Object.prototype.toString.call(e), "[object DOMException]", "toStringTag");
  assert(!Object.prototype.hasOwnProperty.call(DOMException.prototype, "toString"), "no own toString");
  same(e.toString(), "name: message", "Error.prototype.toString");
  Object.defineProperty(e, "name", { value: "new name" });
  Object.defineProperty(e, "message", { value: "new message" });
  same(e.toString(), "new name: new message", "shadowed");
  throwsType(TypeError, () => DOMException.prototype.toString(), "on the prototype");
});

test("DOMException: is an Error with a stack", () => {
  const e = new DOMException("m", "AbortError");
  assert(e instanceof Error && e instanceof DOMException, "instanceof");
  same(typeof e.stack, typeof new Error("x").stack, "stack type");
  same(Error.prototype.toString.call(e), "AbortError: m", "toString");
});

test("DOMException: subclasses", () => {
  class MyError extends DOMException {
    constructor(message) {
      super(message, "DataError");
      this.extra = 1;
    }
  }
  const e = new MyError("sub");
  assert(e instanceof MyError && e instanceof DOMException && e instanceof Error, "chain");
  same(e.name, "DataError", "name");
  same(e.message, "sub", "message");
  same(e.extra, 1, "own field");
});

test("DOMException: ToString of the arguments", () => {
  throwsType(TypeError, () => new DOMException(Symbol("s")), "symbol message");
  throwsType(TypeError, () => new DOMException("m", Symbol("s")), "symbol name");
  same(new DOMException({ toString: () => "from toString" }).message, "from toString", "object");
});

// ---------------------------------------------------------------------
// atob / btoa
// ---------------------------------------------------------------------

function referenceBtoa(s) {
  const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  let out = "";
  for (let i = 0; i < s.length; i += 3) {
    const a = s.charCodeAt(i);
    const b = i + 1 < s.length ? s.charCodeAt(i + 1) : NaN;
    const c = i + 2 < s.length ? s.charCodeAt(i + 2) : NaN;
    out += alphabet[a >> 2];
    out += alphabet[((a & 3) << 4) | (b >> 4)];
    out += Number.isNaN(b) ? "=" : alphabet[((b & 15) << 2) | (c >> 6)];
    out += Number.isNaN(c) ? "=" : alphabet[c & 63];
  }
  return out;
}

test("btoa: WPT inputs", () => {
  const inputs = ["", "ab", "abc", "abcd", "abcde", "\xff\xff\xc0", "\0a", "a\0b",
                  undefined, null, 7, 12, 1.5, true, false, NaN, Infinity, -Infinity, 0, -0,
                  { toString() { return "foo"; } }];
  for (let i = 0; i < 256; i++) {
    inputs.push(String.fromCharCode(i));
  }
  let all = "";
  for (let i = 0; i < 256; i++) {
    all += String.fromCharCode(i);
  }
  inputs.push(all);
  for (const input of inputs) {
    const s = String(input);
    same(btoa(input), referenceBtoa(s), `btoa(${hex(s.slice(0, 8))})`);
    same(atob(btoa(input)), s, `atob(btoa(${hex(s.slice(0, 8))}))`);
  }
});

test("btoa: characters above U+00FF", () => {
  for (const s of ["\u05E2\u05D1\u05E8\u05D9\u05EA", String.fromCharCode(256),
                   String.fromCharCode(10000), String.fromCharCode(65534),
                   String.fromCharCode(65535), "\u{10000}"]) {
    throwsDom("InvalidCharacterError", () => btoa(s), hex(s));
  }
  const e = throwsDom("InvalidCharacterError", () => btoa("\u0100"), "code");
  same(e.code, 5, "legacy code");
});

test("atob: WebIDL conversions", () => {
  const cases = [
    [undefined, null], [null, [158, 233, 101]], [7, null], [12, [215]], [1.5, null],
    [true, [182, 187]], [false, null], [NaN, [53, 163]], [Infinity, [34, 119, 226, 158, 43, 114]],
    [-Infinity, null], [0, null], [-0, null],
    [{ toString() { return "foo"; } }, [126, 138]],
    [{ toString() { return "abcd"; } }, [105, 183, 29]],
  ];
  for (const [input, output] of cases) {
    if (output === null) {
      throwsDom("InvalidCharacterError", () => atob(input), String(input));
    } else {
      // As in WPT, only the listed leading bytes are compared.
      const result = atob(input);
      output.forEach((b, i) => same(result.charCodeAt(i), b, `${String(input)}[${i}]`));
    }
  }
});

test("atob: forgiving-base64", () => {
  const ok = [["", ""], ["abcd", "i\xb7\x1d"], [" abcd", "i\xb7\x1d"], ["abcd ", "i\xb7\x1d"],
              ["ab=", null], ["ab==", "i"], ["abc=", "i\xb7"], ["ab", "i"], ["abc", "i\xb7"],
              ["a", null], ["ab\t\n\f\r cd", "i\xb7\x1d"], ["ab\vcd", null], ["ab\xa0cd", null],
              ["ab\u3000cd", null], ["abcd===", null], ["a=b=", null], ["ab=c", null],
              ["ab/+", "i\xbf\xfe"], ["ab-_", null], ["YR", "a"], ["YQ", "a"], ["YQ===", null],
              ["====", null], ["=", null], ["AAAA", "\0\0\0"]];
  for (const [input, expected] of ok) {
    if (expected === null) {
      throwsDom("InvalidCharacterError", () => atob(input), JSON.stringify(input));
    } else {
      same(atob(input), expected, JSON.stringify(input));
    }
  }
});

// ---------------------------------------------------------------------
// TextEncoder
// ---------------------------------------------------------------------

test("TextEncoder: basics", () => {
  const e = new TextEncoder();
  same(e.encoding, "utf-8", "encoding");
  sameBytes(e.encode(), [], "no input");
  sameBytes(e.encode(undefined), [], "undefined");
  sameBytes(e.encode("z\xA2\u6C34\u{1D11E}\uF8FF\u{10FFFD}\uFFFE"),
            [0x7A, 0xC2, 0xA2, 0xE6, 0xB0, 0xB4, 0xF0, 0x9D, 0x84, 0x9E, 0xEF, 0xA3, 0xBF,
             0xF4, 0x8F, 0xBF, 0xBD, 0xEF, 0xBF, 0xBE], "sample");
  sameBytes(e.encode("\uD800"), [0xEF, 0xBF, 0xBD], "lone surrogate");
  sameBytes(e.encode(null), [0x6E, 0x75, 0x6C, 0x6C], "null");
  assert(e.encode("x") instanceof Uint8Array, "Uint8Array");
  throwsType(TypeError, () => e.encode(Symbol()), "symbol");
  same(TextEncoder.prototype.encode.length, 0, "encode.length");
  same(TextEncoder.prototype.encodeInto.length, 2, "encodeInto.length");
});

test("TextEncoder: members", () => {
  for (const key of ["encoding", "encode", "encodeInto"]) {
    const d = Object.getOwnPropertyDescriptor(TextEncoder.prototype, key);
    assert(d.enumerable && d.configurable, key + " enumerable");
  }
  throwsType(TypeError, () => TextEncoder(), "call");
  throwsType(TypeError, () => TextEncoder.prototype.encode.call({}, "a"), "brand: encode");
  throwsType(TypeError, () => Object.getOwnPropertyDescriptor(TextEncoder.prototype, "encoding").get.call(1),
             "brand: encoding on a number");
  throwsType(TypeError, () => TextEncoder.prototype.encodeInto.call(new TextDecoder(), "", new Uint8Array(1)),
             "brand: encodeInto on a TextDecoder");
});

test("TextEncoder: encodeInto (WPT table)", () => {
  const table = [
    ["Hi", 0, 0, []],
    ["A", 10, 1, [0x41]],
    ["\u{1D306}", 4, 2, [0xF0, 0x9D, 0x8C, 0x86]],
    ["\u{1D306}A", 3, 0, []],
    ["\uD834A\uDF06A\xA5Hi", 10, 5, [0xEF, 0xBF, 0xBD, 0x41, 0xEF, 0xBF, 0xBD, 0x41, 0xC2, 0xA5]],
    ["A\uDF06", 4, 2, [0x41, 0xEF, 0xBF, 0xBD]],
    ["\xA5\xA5", 4, 2, [0xC2, 0xA5, 0xC2, 0xA5]],
  ];
  for (const [input, destinationLength, read, written] of table) {
    for (const [increase, offset, filler] of [[0, 0, 0], [10, 4, 0], [0, 0, 0x80], [10, 4, 0x80]]) {
      const buffer = new ArrayBuffer(destinationLength + increase);
      const view = new Uint8Array(buffer, offset, destinationLength);
      const full = new Uint8Array(buffer);
      full.fill(filler);
      const result = new TextEncoder().encodeInto(input, view);
      same(result.read, read, `${hex(input)} read`);
      same(result.written, written.length, `${hex(input)} written`);
      assert(Object.getPrototypeOf(result) === Object.prototype, "a plain object");
      for (let i = 0; i < full.length; i++) {
        const expected = i >= offset && i < offset + written.length ? written[i - offset] : filler;
        same(full[i], expected, `${hex(input)} byte ${i}`);
      }
    }
  }
});

test("TextEncoder: encodeInto destinations", () => {
  const e = new TextEncoder();
  for (const make of [() => new DataView(new ArrayBuffer(0)), () => new Int8Array(0),
                      () => new Int16Array(0), () => new Uint16Array(0), () => new Uint8ClampedArray(0),
                      () => new Float64Array(0), () => new BigInt64Array(0), () => new ArrayBuffer(10),
                      () => [], () => null, () => undefined]) {
    throwsType(TypeError, () => e.encodeInto("", make()), Object.prototype.toString.call(make()));
  }
  throwsType(TypeError, () => e.encodeInto("a"), "missing destination");
  const buffer = new ArrayBuffer(10);
  const view = new Uint8Array(buffer);
  if (typeof buffer.transfer === "function") {
    buffer.transfer();
    const r = e.encodeInto("test", view);
    same(r.read, 0, "detached read");
    same(r.written, 0, "detached written");
  }
});

// ---------------------------------------------------------------------
// TextDecoder
// ---------------------------------------------------------------------

test("TextDecoder: basics", () => {
  const d = new TextDecoder();
  same(d.encoding, "utf-8", "encoding");
  same(d.fatal, false, "fatal");
  same(d.ignoreBOM, false, "ignoreBOM");
  same(new TextDecoder("utf-8", { fatal: true }).fatal, true, "fatal option");
  same(new TextDecoder("utf-8", { ignoreBOM: 1 }).ignoreBOM, true, "ignoreBOM converts");
  same(new TextDecoder(undefined, null).encoding, "utf-8", "undefined label, null options");
  throwsType(TypeError, () => new TextDecoder("utf-8", 5), "options not an object");
  throwsType(TypeError, () => TextDecoder(), "call");
  same(d.decode(), "", "no input");
  same(TextDecoder.prototype.decode.length, 0, "decode.length");
  for (const key of ["encoding", "fatal", "ignoreBOM", "decode"]) {
    const desc = Object.getOwnPropertyDescriptor(TextDecoder.prototype, key);
    assert(desc.enumerable && desc.configurable, key + " enumerable");
  }
  throwsType(TypeError, () => TextDecoder.prototype.decode.call({}), "brand: decode");
  throwsType(TypeError, () => Object.getOwnPropertyDescriptor(TextDecoder.prototype, "fatal").get.call(new TextEncoder()),
             "brand: fatal on a TextEncoder");
});

test("TextDecoder: inputs", () => {
  const sample = "z\xA2\u6C34\u{1D11E}\uF8FF\u{10FFFD}\uFFFE";
  const utf8 = [0x7A, 0xC2, 0xA2, 0xE6, 0xB0, 0xB4, 0xF0, 0x9D, 0x84, 0x9E, 0xEF, 0xA3, 0xBF,
                0xF4, 0x8F, 0xBF, 0xBD, 0xEF, 0xBF, 0xBE];
  const u8 = bytes(utf8);
  const d = new TextDecoder();
  sameText(d.decode(u8), sample, "Uint8Array");
  sameText(d.decode(u8.buffer), sample, "ArrayBuffer");
  sameText(d.decode(new DataView(u8.buffer)), sample, "DataView");
  sameText(d.decode(new Uint16Array(u8.buffer, 0, 10)), sample, "Uint16Array");
  sameText(d.decode(u8.subarray(1, 3)), "\xA2", "view with an offset");
  for (const bad of [null, "text", 1, {}, [65], Symbol()]) {
    throwsType(TypeError, () => d.decode(bad), "input " + typeof bad);
  }
  let touched = false;
  throwsType(TypeError, () => d.decode("x", { get stream() { touched = true; return false; } }),
             "input checked first");
  same(touched, false, "options not read when the input is wrong");
  const buffer = new ArrayBuffer(4);
  if (typeof buffer.transfer === "function") {
    const view = new Uint8Array(buffer);
    buffer.transfer();
    same(d.decode(view), "", "detached view");
    same(d.decode(buffer), "", "detached buffer");
  }
});

test("TextDecoder: UTF-16 samples", () => {
  const sample = "z\xA2\u6C34\u{1D11E}\uF8FF\u{10FFFD}\uFFFE";
  const le = [0x7A, 0x00, 0xA2, 0x00, 0x34, 0x6C, 0x34, 0xD8, 0x1E, 0xDD, 0xFF, 0xF8, 0xFF, 0xDB,
              0xFD, 0xDF, 0xFE, 0xFF];
  const be = [0x00, 0x7A, 0x00, 0xA2, 0x6C, 0x34, 0xD8, 0x34, 0xDD, 0x1E, 0xF8, 0xFF, 0xDB, 0xFF,
              0xDF, 0xFD, 0xFF, 0xFE];
  sameText(new TextDecoder("utf-16le").decode(bytes(le)), sample, "utf-16le");
  sameText(new TextDecoder("utf-16be").decode(bytes(be)), sample, "utf-16be");
  sameText(new TextDecoder("utf-16").decode(bytes(le)), sample, "utf-16");
  same(new TextDecoder("utf-16").encoding, "utf-16le", "utf-16 is utf-16le");
});

test("TextDecoder: fatal (WPT table)", () => {
  const bad = [
    [0xFF], [0xC0], [0xE0], [0xC0, 0x00], [0xC0, 0xC0], [0xE0, 0x00], [0xE0, 0xC0],
    [0xE0, 0x80, 0x00], [0xE0, 0x80, 0xC0], [0xFC, 0x80, 0x80, 0x80, 0x80, 0x80],
    [0xFE, 0x80, 0x80, 0x80, 0x80, 0x80], [0xC0, 0x80], [0xE0, 0x80, 0x80],
    [0xF0, 0x80, 0x80, 0x80], [0xF8, 0x80, 0x80, 0x80, 0x80], [0xC1, 0xBF], [0xE0, 0x81, 0xBF],
    [0xF0, 0x80, 0x81, 0xBF], [0xE0, 0x9F, 0xBF], [0xF0, 0x80, 0x9F, 0xBF], [0xF0, 0x8F, 0xBF, 0xBF],
    [0xF8, 0x84, 0x8F, 0xBF, 0xBF], [0xED, 0xA0, 0x80], [0xED, 0xB0, 0x80],
    [0xED, 0xA0, 0x80, 0xED, 0xB0, 0x80],
  ];
  for (const input of bad) {
    throwsType(TypeError, () => new TextDecoder("utf-8", { fatal: true }).decode(bytes(input)),
               "utf-8 " + input.map((b) => b.toString(16)).join(" "));
  }
  throwsType(TypeError, () => new TextDecoder("utf-16le", { fatal: true }).decode(bytes([0x00])),
             "utf-16le truncated code unit");
});

test("TextDecoder: a fatal error does not stop later decodes", () => {
  const heart = bytes([226, 153, 165]);
  const decoder = new TextDecoder("utf-8", { fatal: true });
  same(decoder.decode(new DataView(heart.buffer, 0, 3)), "\u2665", "first");
  throwsType(TypeError, () => decoder.decode(new DataView(heart.buffer, 0, 2)), "incomplete");
  same(decoder.decode(new DataView(heart.buffer, 0, 3)), "\u2665", "after the error");
  throwsType(TypeError, () => decoder.decode(bytes([0xE2, 0x99]), { stream: true }).concat(decoder.decode()),
             "incomplete at the end of a stream");
  same(decoder.decode(heart), "\u2665", "after a stream error");
});

test("TextDecoder: streaming (WPT)", () => {
  const string = "\x00123ABCabc\x80\xFF\u0100\u1000\uFFFD\u{10000}\u{10FFFF}";
  const octets = {
    "utf-8": [0x00, 0x31, 0x32, 0x33, 0x41, 0x42, 0x43, 0x61, 0x62, 0x63, 0xc2, 0x80, 0xc3, 0xbf,
              0xc4, 0x80, 0xe1, 0x80, 0x80, 0xef, 0xbf, 0xbd, 0xf0, 0x90, 0x80, 0x80, 0xf4, 0x8f,
              0xbf, 0xbf],
    "utf-16le": [0x00, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00, 0x41, 0x00, 0x42, 0x00, 0x43, 0x00,
                 0x61, 0x00, 0x62, 0x00, 0x63, 0x00, 0x80, 0x00, 0xFF, 0x00, 0x00, 0x01, 0x00, 0x10,
                 0xFD, 0xFF, 0x00, 0xD8, 0x00, 0xDC, 0xFF, 0xDB, 0xFF, 0xDF],
    "utf-16be": [0x00, 0x00, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00, 0x41, 0x00, 0x42, 0x00, 0x43,
                 0x00, 0x61, 0x00, 0x62, 0x00, 0x63, 0x00, 0x80, 0x00, 0xFF, 0x01, 0x00, 0x10, 0x00,
                 0xFF, 0xFD, 0xD8, 0x00, 0xDC, 0x00, 0xDB, 0xFF, 0xDF, 0xFF],
  };
  for (const encoding of Object.keys(octets)) {
    for (let len = 1; len <= 5; len++) {
      const encoded = octets[encoding];
      const decoder = new TextDecoder(encoding);
      let out = "";
      for (let i = 0; i < encoded.length; i += len) {
        out += decoder.decode(bytes(encoded.slice(i, i + len)), { stream: true });
      }
      out += decoder.decode();
      sameText(out, string, `${encoding}, ${len} byte window`);
    }
  }
});

test("TextDecoder: UTF-8 chunk tests (WPT)", () => {
  const d = new TextDecoder();
  const steps = [
    [[0xC1], true, "\uFFFD"], [[], false, ""], [[0xF5], true, "\uFFFD"], [[], false, ""],
    [[0xE0, 0x41], true, "\uFFFDA"], [[0x42], false, "B"],
    [[0xE0, 0x80], true, "\uFFFD\uFFFD"], [[0x80], false, "\uFFFD"],
    [[0xED, 0xA0], true, "\uFFFD\uFFFD"], [[0x80], false, "\uFFFD"],
    [[0xF0, 0x41], true, "\uFFFDA"], [[0x42], true, "B"], [[0x43], false, "C"],
    [[0xF0, 0x80], true, "\uFFFD\uFFFD"], [[0x80], true, "\uFFFD"], [[0x80], false, "\uFFFD"],
    [[0xF4, 0xA0], true, "\uFFFD\uFFFD"], [[0x80], true, "\uFFFD"], [[0x80], false, "\uFFFD"],
    [[0xF0, 0x90, 0x41], true, "\uFFFDA"], [[0x42], false, "B"],
    [[0xF0, 0x9F, 0x92], true, ""], [[0xA9], false, "\u{1F4A9}"],
  ];
  steps.forEach(([input, stream, expected], i) => {
    const result = input.length === 0 && !stream ? d.decode() : d.decode(bytes(input), { stream });
    sameText(result, expected, `step ${i}`);
  });
});

test("TextDecoder: byte order marks", () => {
  const cases = [
    ["utf-8", [0xEF, 0xBB, 0xBF, 0x61]],
    ["utf-16le", [0xFF, 0xFE, 0x61, 0x00]],
    ["utf-16be", [0xFE, 0xFF, 0x00, 0x61]],
  ];
  for (const [encoding, input] of cases) {
    sameText(new TextDecoder(encoding).decode(bytes(input)), "a", encoding + " stripped");
    sameText(new TextDecoder(encoding, { ignoreBOM: true }).decode(bytes(input)), "\uFEFFa",
             encoding + " kept with ignoreBOM");
    const d = new TextDecoder(encoding);
    let out = "";
    for (const b of input) {
      out += d.decode(bytes([b]), { stream: true });
    }
    out += d.decode();
    sameText(out, "a", encoding + " stripped while streaming");
    sameText(d.decode(bytes(input)), "a", encoding + " stripped again after the end");
  }
  const d = new TextDecoder();
  sameText(d.decode(bytes([0x61, 0xEF, 0xBB, 0xBF]), { stream: true }) + d.decode(bytes([0xEF, 0xBB, 0xBF])),
           "a\uFEFF\uFEFF", "only a leading BOM is removed");
  sameText(new TextDecoder("windows-1252").decode(bytes([0xEF, 0xBB, 0xBF])), "\xEF\xBB\xBF",
           "no BOM handling for a single-byte encoding");
  sameText(new TextDecoder("utf-16le").decode(bytes([0xEF, 0xBB, 0xBF, 0x00])), "\uBBEF\xBF",
           "a UTF-8 BOM means nothing to UTF-16");
});

test("TextDecoder: labels", () => {
  const labels = [
    ["unicode-1-1-utf-8", "utf-8"], ["unicode11utf8", "utf-8"], ["unicode20utf8", "utf-8"],
    ["utf8", "utf-8"], ["x-unicode20utf8", "utf-8"], ["latin1", "windows-1252"],
    ["ascii", "windows-1252"], ["us-ascii", "windows-1252"], ["iso-8859-1", "windows-1252"],
    ["csunicode", "utf-16le"], ["ucs-2", "utf-16le"], ["unicodefffe", "utf-16be"],
    ["koi8-ru", "koi8-u"], ["iso-8859-8-i", "iso-8859-8-i"], ["logical", "iso-8859-8-i"],
    ["visual", "iso-8859-8"], ["x-user-defined", "x-user-defined"], ["cp1251", "windows-1251"],
    ["x-mac-roman", "macintosh"], ["tis-620", "windows-874"], ["ISO-8859-15", "iso-8859-15"],
  ];
  for (const [label, name] of labels) {
    same(new TextDecoder(label).encoding, name, label);
    same(new TextDecoder(label.toUpperCase()).encoding, name, label + " upper case");
    for (const ws of [" ", "\t", "\n", "\f", "\r"]) {
      same(new TextDecoder(ws + label + ws).encoding, name, label + " with whitespace");
    }
  }
  for (const ws of ["\u0000", "\u000b", "\xA0", "\u2028", "\u2029"]) {
    throwsType(RangeError, () => new TextDecoder(ws + "utf-8"), "leading " + hex(ws));
    throwsType(RangeError, () => new TextDecoder("utf-8" + ws), "trailing " + hex(ws));
  }
  for (const label of ["", "invalid-invalidLabel", "utf-7", "replacement", "iso-2022-kr",
                       "hz-gb-2312", "utf-32", "UTF-8 "]) {
    if (label === "UTF-8 ") {
      same(new TextDecoder(label).encoding, "utf-8", "trailing space is trimmed");
      continue;
    }
    throwsType(RangeError, () => new TextDecoder(label), "label " + JSON.stringify(label));
  }
  const e = throwsType(RangeError, () => new TextDecoder("shift_jis"), "CJK");
  assert(e.message.includes("does not decode"), "CJK message says it is not supported yet");
  let order = "";
  throwsType(RangeError, () => new TextDecoder({ toString() { order += "label,"; return "bogus"; } },
                                               { get fatal() { order += "fatal,"; return false; },
                                                 get ignoreBOM() { order += "ignoreBOM"; return false; } }),
             "bogus label");
  same(order, "label,fatal,ignoreBOM", "conversion order: label, then options, then the lookup");
});

test("TextDecoder: single-byte encodings", () => {
  const all = new Uint8Array(256);
  for (let i = 0; i < 256; i++) {
    all[i] = i;
  }
  const w1252 = new TextDecoder("windows-1252").decode(all);
  same(w1252.length, 256, "length");
  same(w1252.charCodeAt(0x80), 0x20AC, "0x80 is the euro sign");
  same(w1252.charCodeAt(0x81), 0x81, "0x81 maps to itself");
  same(w1252.charCodeAt(0x9F), 0x178, "0x9F");
  same(w1252.charCodeAt(0xE9), 0xE9, "0xE9");
  const user = new TextDecoder("x-user-defined").decode(all);
  same(user.charCodeAt(0x7F), 0x7F, "x-user-defined ASCII");
  same(user.charCodeAt(0x80), 0xF780, "x-user-defined 0x80");
  same(user.charCodeAt(0xFF), 0xF7FF, "x-user-defined 0xFF");
  same(new TextDecoder("iso-8859-3").decode(bytes([0xA5])), "\uFFFD", "unmapped byte");
  throwsType(TypeError, () => new TextDecoder("iso-8859-3", { fatal: true }).decode(bytes([0xA5])),
             "unmapped byte, fatal");
  same(new TextDecoder("koi8-u").decode(bytes([0xAE])), "\u045E", "koi8-u 0xAE (WHATWG)");
  same(new TextDecoder("windows-1255").decode(bytes([0xCA])), "\u05BA", "windows-1255 0xCA (WHATWG)");
});

test("TextDecoder: large input", () => {
  const big = new Uint8Array(1 << 20).fill(0x61);
  same(new TextDecoder().decode(big).length, 1 << 20, "1 MiB of ASCII");
  const text = "\xE9\u20AC\u{1F600}".repeat(50000);
  same(new TextDecoder().decode(new TextEncoder().encode(text)), text, "round trip");
});

// ---------------------------------------------------------------------
// Primordials: user code changing built-ins cannot break or observe the
// APIs (rules 1 and 2 of runtime/WebApis.js).
// ---------------------------------------------------------------------

test("primordials: patched built-ins are not used", () => {
  const saved = {
    get: WeakMap.prototype.get, set: WeakMap.prototype.set, slice: String.prototype.slice,
    call: Function.prototype.call, defineProperty: Object.defineProperty,
    iterator: Array.prototype[Symbol.iterator],
  };
  let calls = 0;
  const withBom = bytes([0xEF, 0xBB, 0xBF, 0x61]);
  try {
    WeakMap.prototype.get = function () { calls++; return undefined; };
    WeakMap.prototype.set = function () { calls++; return this; };
    String.prototype.slice = function () { calls++; return "patched"; };
    Function.prototype.call = function () { calls++; throw new Error("patched call"); };
    Array.prototype[Symbol.iterator] = function () { calls++; throw new Error("patched iterator"); };
    Object.defineProperty(Object.prototype, "value", { set() { calls++; }, configurable: true });
    const e = new DOMException("m", "AbortError");
    same(e.name, "AbortError", "name");
    same(e.code, 20, "code");
    same(new TextDecoder().decode(withBom), "a", "BOM strip");
    same(new TextEncoder().encode("ab").length, 2, "encode");
    same(atob("YQ=="), "a", "atob");
  } finally {
    WeakMap.prototype.get = saved.get;
    WeakMap.prototype.set = saved.set;
    String.prototype.slice = saved.slice;
    Function.prototype.call = saved.call;
    Array.prototype[Symbol.iterator] = saved.iterator;
    delete Object.prototype.value;
  }
  same(calls, 0, "patched functions called");
});

test("primordials: replaced globals are not used", () => {
  const savedError = globalThis.TypeError;
  const savedDom = globalThis.DOMException;
  try {
    globalThis.TypeError = function Fake() {};
    globalThis.DOMException = function Fake() {};
    let thrown;
    try { atob("*"); } catch (e) { thrown = e; }
    assert(thrown instanceof savedDom, "atob still throws the real DOMException");
    try { TextDecoder.prototype.decode.call({}); } catch (e) { thrown = e; }
    assert(thrown instanceof savedError, "brand failures throw the real TypeError");
  } finally {
    globalThis.TypeError = savedError;
    globalThis.DOMException = savedDom;
  }
});

test("errors point at the caller", () => {
  let e;
  try { atob("*"); } catch (caught) { e = caught; }
  assert(typeof e.stack === "string", "stack");
  assert(!e.stack.includes("forge:web"), "no Forge-internal frame first: " + e.stack.split("\n")[0]);
  try { TextDecoder.prototype.decode.call({}); } catch (caught) { e = caught; }
  assert(!e.stack.split("\n")[0].includes("forge:web"), "TypeError: " + e.stack.split("\n")[0]);
});

console.log(`web done: ${checks} checks, ${failures} failed`);
if (failures !== 0) {
  Forge.exitCode = 1;
}
