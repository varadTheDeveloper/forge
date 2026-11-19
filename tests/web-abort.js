// Phase 10.3: AbortController, AbortSignal (abort, timeout, any,
// throwIfAborted, reason, onabort) and the `signal` option of
// addEventListener.
//
// The checks follow the Web Platform Tests for these APIs (dom/abort/,
// dom/events/AddEventListenerOptions-signal.any.js), transcribed by hand,
// plus Forge's own rules (PHASE10_DESIGN.md). Every failure prints a line
// starting with "ABORT FAIL"; the last line is
// "abort done: <checks> checks, <failures> failed".
//
// Which WPT subtest each check covers, at which WPT commit, is recorded in
// tests/wpt/coverage.json (decision D29; tools/wpt-coverage.py). Cases
// transcribed from WPT are under WPT's 3-Clause BSD License
// (tests/wpt/LICENSE.md).

"use strict";

let checks = 0;
let failures = 0;

function fail(name, e) {
  failures++;
  console.log(`ABORT FAIL ${name}: ${e && e.message ? e.message : String(e)}`);
}

function test(name, body) {
  checks++;
  try {
    body();
  } catch (e) {
    fail(name, e);
  }
}

const pending = [];

// An asynchronous check: `body` returns a promise.
function asyncTest(name, body) {
  checks++;
  pending.push(Promise.resolve().then(body).catch((e) => fail(name, e)));
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

function throwsExactly(value, body, what) {
  try {
    body();
  } catch (e) {
    same(e, value, what);
    return;
  }
  throw new Error(`${what}: did not throw`);
}

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// ---------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------

for (const name of ["AbortController", "AbortSignal"]) {
  test(`${name} global property`, () => {
    const d = Object.getOwnPropertyDescriptor(globalThis, name);
    assert(d && typeof d.value === "function", "exists");
    assert(d.writable && !d.enumerable && d.configurable, "attributes");
    same(Object.prototype.toString.call(d.value.prototype), `[object ${name}]`, "toStringTag");
  });
}

test("AbortSignal inherits from EventTarget", () => {
  same(Object.getPrototypeOf(AbortSignal), EventTarget, "interface object");
  same(Object.getPrototypeOf(AbortSignal.prototype), EventTarget.prototype, "prototype");
  const signal = new AbortController().signal;
  assert(signal instanceof AbortSignal && signal instanceof EventTarget, "instanceof");
});

test("AbortSignal members", () => {
  same(Object.keys(AbortSignal).join(), "abort,timeout,any", "static operations, enumerable");
  same(Object.keys(AbortSignal.prototype).join(), "aborted,reason,throwIfAborted,onabort", "members");
  same(Object.keys(AbortController.prototype).join(), "signal,abort", "controller members");
  same(AbortSignal.length, 0, "AbortSignal.length");
  same(AbortSignal.abort.length, 0, "abort.length");
  same(AbortSignal.timeout.length, 1, "timeout.length");
  same(AbortSignal.any.length, 1, "any.length");
  same(AbortController.length, 0, "AbortController.length");
  same(AbortController.prototype.abort.length, 0, "controller abort.length");
});

test("AbortSignal has no constructor", () => {
  throwsType(TypeError, () => new AbortSignal(), "new AbortSignal()");
  throwsType(TypeError, () => AbortSignal(), "AbortSignal()");
  throwsType(TypeError, () => { class Mine extends AbortSignal {} new Mine(); }, "subclass");
});

test("brand checks", () => {
  const aborted = Object.getOwnPropertyDescriptor(AbortSignal.prototype, "aborted").get;
  const onabort = Object.getOwnPropertyDescriptor(AbortSignal.prototype, "onabort");
  throwsType(TypeError, () => aborted.call({}), "aborted");
  throwsType(TypeError, () => onabort.get.call(new EventTarget()), "onabort getter");
  throwsType(TypeError, () => onabort.set.call(new EventTarget(), () => {}), "onabort setter");
  throwsType(TypeError, () => AbortSignal.prototype.throwIfAborted.call({}), "throwIfAborted");
  throwsType(TypeError, () => AbortController.prototype.abort.call({}), "abort");
  const signal = Object.getOwnPropertyDescriptor(AbortController.prototype, "signal").get;
  throwsType(TypeError, () => signal.call(new AbortController().signal), "signal");
});

test("a subclass of AbortController", () => {
  class Mine extends AbortController {}
  const c = new Mine();
  assert(c.signal instanceof AbortSignal, "signal");
  c.abort("sub");
  same(c.signal.reason, "sub", "reason");
});

// ---------------------------------------------------------------------
// dom/abort/event.any.js
// ---------------------------------------------------------------------

test("AbortController abort() should fire event synchronously", () => {
  const c = new AbortController();
  const s = c.signal;
  let state = "begin";
  same(s.aborted, false, "aborted");
  assert("reason" in s, "has reason");
  same(s.reason, undefined, "reason");
  s.addEventListener("abort", () => {
    same(state, "begin", "state");
    state = "aborted";
  });
  c.abort();
  same(state, "aborted", "fired");
  same(s.aborted, true, "aborted after");
  assert(s.reason instanceof DOMException, "reason is a DOMException");
  same(s.reason.name, "AbortError", "AbortError");
  same(s.reason.message, "signal is aborted without reason", "message");
  same(s.reason, s.reason, "the same DOMException each time");
  c.abort();
});

test("controller.signal should always return the same object", () => {
  const c = new AbortController();
  const s = c.signal;
  same(c.signal, s, "before");
  c.abort();
  same(c.signal, s, "after");
});

test("controller.abort() should do nothing the second time it is called", () => {
  const c = new AbortController();
  let count = 0;
  c.signal.onabort = () => { count++; };
  c.abort();
  same(count, 1, "once");
  c.abort();
  same(count, 1, "not again");
});

test("event handler should not be called if added after controller.abort()", () => {
  const c = new AbortController();
  c.abort();
  c.signal.onabort = () => { throw new Error("called"); };
});

test("the abort event should have the right properties", () => {
  const c = new AbortController();
  let seen = false;
  c.signal.onabort = (e) => {
    seen = true;
    same(e.type, "abort", "type");
    same(e.target, c.signal, "target");
    same(e.bubbles, false, "bubbles");
    same(e.cancelable, false, "cancelable");
    same(e.isTrusted, true, "isTrusted");
    assert(e.constructor === Event, "an Event");
  };
  c.abort();
  assert(seen, "fired");
});

test("abort(reason) sets signal.reason; undefined is the default, null is a reason", () => {
  const reason = Error("hello");
  const c1 = new AbortController();
  c1.abort(reason);
  same(c1.signal.reason, reason, "reason");
  const c2 = new AbortController();
  c2.abort(undefined);
  same(c2.signal.reason.name, "AbortError", "undefined");
  const c3 = new AbortController();
  c3.abort(null);
  same(c3.signal.reason, null, "null");
});

test("AbortSignal.abort()", () => {
  const s = AbortSignal.abort();
  same(s.aborted, true, "aborted");
  same(s.reason.name, "AbortError", "AbortError");
  same(s.reason, s.reason, "the same DOMException");
  const reason = Error("hello");
  same(AbortSignal.abort(reason).reason, reason, "with a reason");
  let fired = false;
  AbortSignal.abort().onabort = () => { fired = true; };
  same(fired, false, "no event");
});

asyncTest("signal returned by AbortSignal.abort() should not fire abort event", async () => {
  const s = AbortSignal.abort();
  let fired = false;
  s.addEventListener("abort", () => { fired = true; });
  s.onabort = () => { fired = true; };
  await delay(30);
  same(fired, false, "no abort event, later either");
});

test("throwIfAborted()", () => {
  const reason = new Error("boom");
  throwsExactly(reason, () => AbortSignal.abort(reason).throwIfAborted(), "object reason");
  same(reason.message, "boom", "reason unchanged");
  throwsExactly("hello", () => AbortSignal.abort("hello").throwIfAborted(), "primitive reason");
  same(new AbortController().signal.throwIfAborted(), undefined, "not aborted");
});

// ---------------------------------------------------------------------
// The onabort event handler
// ---------------------------------------------------------------------

test("onabort keeps its place among the listeners", () => {
  const c = new AbortController();
  let order = "";
  c.signal.addEventListener("abort", () => { order += "1"; });
  c.signal.onabort = () => { order += "x"; };
  c.signal.addEventListener("abort", () => { order += "3"; });
  c.signal.onabort = () => { order += "2"; };
  same(c.signal.onabort.toString().includes('"2"'), true, "getter returns the last value");
  c.abort();
  same(order, "123", "order");
});

test("onabort = null removes it; a non-object is null", () => {
  const c = new AbortController();
  let count = 0;
  c.signal.onabort = () => { count++; };
  c.signal.onabort = null;
  same(c.signal.onabort, null, "null");
  c.signal.onabort = 5;
  same(c.signal.onabort, null, "a number is null");
  c.abort();
  same(count, 0, "removed");
});

test("onabort on two signals is independent", () => {
  const a = new AbortController();
  const b = new AbortController();
  let seen = "";
  a.signal.onabort = () => { seen += "a"; };
  b.signal.onabort = () => { seen += "b"; };
  b.abort();
  same(seen, "b", "only b");
  same(a.signal.onabort !== b.signal.onabort, true, "different handlers");
});

test("a throwing abort listener is reported and the rest still run", () => {
  const errors = [];
  const onError = (e) => { errors.push(e.message); e.preventDefault(); };
  addEventListener("error", onError);
  const c = new AbortController();
  let after = false;
  c.signal.onabort = {};  // not callable: a TypeError when it runs
  c.signal.addEventListener("abort", () => { throw new Error("listener boom"); });
  c.signal.addEventListener("abort", () => { after = true; });
  c.abort();
  removeEventListener("error", onError);
  same(errors.length, 2, "two errors reported");
  assert(errors[1].includes("listener boom"), "the listener's error");
  same(after, true, "later listener ran");
  same(c.signal.aborted, true, "aborted");
});

// ---------------------------------------------------------------------
// dom/events/AddEventListenerOptions-signal.any.js
// ---------------------------------------------------------------------

test("Passing an AbortSignal to addEventListener options should allow removing a listener", () => {
  let count = 0;
  const handler = () => { count++; };
  const et = new EventTarget();
  const c = new AbortController();
  et.addEventListener("test", handler, { signal: c.signal });
  et.dispatchEvent(new Event("test"));
  same(count, 1, "added");
  et.dispatchEvent(new Event("test"));
  same(count, 2, "not once");
  c.abort();
  et.dispatchEvent(new Event("test"));
  same(count, 2, "removed by abort");
  et.addEventListener("test", handler, { signal: c.signal });
  et.dispatchEvent(new Event("test"));
  same(count, 2, "an aborted signal never adds the handler");
});

test("Passing an AbortSignal to addEventListener does not prevent removeEventListener", () => {
  let count = 0;
  const handler = () => { count++; };
  const et = new EventTarget();
  const c = new AbortController();
  et.addEventListener("test", handler, { signal: c.signal });
  et.removeEventListener("test", handler);
  et.dispatchEvent(new Event("test"));
  same(count, 0, "removed");
});

test("Passing an AbortSignal to addEventListener works with the once flag", () => {
  let count = 0;
  const handler = () => { count++; };
  const et = new EventTarget();
  const c = new AbortController();
  et.addEventListener("test", handler, { signal: c.signal, once: true });
  c.abort();
  et.dispatchEvent(new Event("test"));
  same(count, 0, "removed");
});

test("Removing a once listener works with a passed signal", () => {
  let count = 0;
  const handler = () => { count++; };
  const et = new EventTarget();
  const c = new AbortController();
  et.addEventListener("test", handler, { signal: c.signal, once: true });
  et.removeEventListener("test", handler);
  et.dispatchEvent(new Event("test"));
  same(count, 0, "removed");
});

test("Passing an AbortSignal to multiple listeners", () => {
  let count = 0;
  const handler = () => { count++; };
  const et = new EventTarget();
  const c = new AbortController();
  et.addEventListener("first", handler, { signal: c.signal, once: true });
  et.addEventListener("second", handler, { signal: c.signal, once: true });
  c.abort();
  et.dispatchEvent(new Event("first"));
  et.dispatchEvent(new Event("second"));
  same(count, 0, "removed");
});

test("Passing an AbortSignal to addEventListener works with the capture flag", () => {
  let count = 0;
  const handler = () => { count++; };
  const et = new EventTarget();
  const c = new AbortController();
  et.addEventListener("test", handler, { signal: c.signal, capture: true });
  c.abort();
  et.dispatchEvent(new Event("test"));
  same(count, 0, "removed");
});

test("Aborting from a listener does not call future listeners", () => {
  let count = 0;
  const et = new EventTarget();
  const c = new AbortController();
  et.addEventListener("test", () => { c.abort(); }, { signal: c.signal });
  et.addEventListener("test", () => { count++; }, { signal: c.signal });
  et.dispatchEvent(new Event("test"));
  same(count, 0, "not called");
});

test("Adding then aborting a listener in another listener does not call it", () => {
  let count = 0;
  const et = new EventTarget();
  const c = new AbortController();
  et.addEventListener("test", () => {
    et.addEventListener("test", () => { count++; }, { signal: c.signal });
    c.abort();
  }, { signal: c.signal });
  et.dispatchEvent(new Event("test"));
  same(count, 0, "not called");
});

test("Aborting from a nested listener should remove it", () => {
  const et = new EventTarget();
  const c = new AbortController();
  let count = 0;
  et.addEventListener("foo", () => {
    et.addEventListener("foo", () => {
      count++;
      if (count > 5) {
        c.abort();
      }
      et.dispatchEvent(new Event("foo"));
    }, { signal: c.signal });
    et.dispatchEvent(new Event("foo"));
  }, { once: true });
  et.dispatchEvent(new Event("foo"));
  same(count, 6, "stopped after the abort");
});

test("Passing null as the signal should throw", () => {
  const et = new EventTarget();
  throwsType(TypeError, () => et.addEventListener("foo", () => {}, { signal: null }), "null");
  throwsType(TypeError, () => et.addEventListener("foo", null, { signal: null }), "null, no listener");
  throwsType(TypeError, () => et.addEventListener("foo", () => {}, { signal: {} }), "an object");
  throwsType(TypeError, () => et.addEventListener("foo", () => {}, { signal: new EventTarget() }),
             "an EventTarget");
});

test("the signal option is read after capture, once and passive", () => {
  const read = [];
  const options = {};
  for (const name of ["signal", "passive", "once", "capture"]) {
    Object.defineProperty(options, name, { get() { read.push(name); return undefined; } });
  }
  new EventTarget().addEventListener("x", () => {}, options);
  same(read.join(), "capture,once,passive,signal", "order");
  read.length = 0;
  new EventTarget().removeEventListener("x", () => {}, options);
  same(read.join(), "capture", "removeEventListener reads capture only");
});

test("a signal also removes the global object's listeners", () => {
  const c = new AbortController();
  let count = 0;
  addEventListener("forge-test", () => { count++; }, { signal: c.signal });
  dispatchEvent(new Event("forge-test"));
  c.abort();
  dispatchEvent(new Event("forge-test"));
  same(count, 1, "removed");
});

test("a duplicate listener with another signal is not removed by it", () => {
  const et = new EventTarget();
  const c1 = new AbortController();
  const c2 = new AbortController();
  let count = 0;
  const handler = () => { count++; };
  et.addEventListener("x", handler, { signal: c1.signal });
  et.addEventListener("x", handler, { signal: c2.signal });  // already there: not added
  c2.abort();
  et.dispatchEvent(new Event("x"));
  same(count, 1, "still there");
  c1.abort();
  et.dispatchEvent(new Event("x"));
  same(count, 1, "removed by its own signal");
});

test("many short-lived listeners on one long-lived signal", () => {
  const c = new AbortController();
  const et = new EventTarget();
  for (let i = 0; i < 1000; i++) {
    const handler = () => {};
    et.addEventListener("x", handler, { signal: c.signal });
    et.removeEventListener("x", handler);
  }
  let count = 0;
  et.addEventListener("x", () => { count++; }, { signal: c.signal });
  c.abort();
  et.dispatchEvent(new Event("x"));
  same(count, 0, "removed");
});

// ---------------------------------------------------------------------
// dom/abort/abort-signal-any.any.js (resources/abort-signal-any-tests.js)
// ---------------------------------------------------------------------

test("AbortSignal.any() works with an empty array of signals", () => {
  same(AbortSignal.any([]).aborted, false, "not aborted");
});

test("AbortSignal.any() follows a single signal", () => {
  const c = new AbortController();
  const clone = AbortSignal.any([c.signal]);
  same(clone.aborted, false, "not aborted");
  same(clone.reason, undefined, "reason");
  assert(clone !== c.signal, "a new signal");
  let fired = false;
  clone.onabort = (e) => {
    same(e.target, clone, "target");
    fired = true;
  };
  c.abort("reason string");
  same(c.signal.aborted, true, "source aborted");
  same(clone.aborted, true, "clone aborted");
  same(fired, true, "fired");
  same(clone.reason, "reason string", "reason propagated");
});

test("AbortSignal.any() follows multiple signals", () => {
  for (let i = 0; i < 3; ++i) {
    const controllers = [new AbortController(), new AbortController(), new AbortController()];
    const combined = AbortSignal.any(controllers.map((c) => c.signal));
    let fired = false;
    combined.onabort = () => { fired = true; };
    controllers[i].abort();
    same(fired, true, `fired for ${i}`);
    same(combined.reason.name, "AbortError", "AbortError");
  }
});

test("AbortSignal.any() returns an aborted signal if passed an aborted signal", () => {
  const controllers = [new AbortController(), new AbortController(), new AbortController()];
  controllers[1].abort("reason 1");
  controllers[2].abort("reason 2");
  const signal = AbortSignal.any(controllers.map((c) => c.signal));
  same(signal.aborted, true, "aborted");
  same(signal.reason, "reason 1", "the first reason");
});

test("AbortSignal.any() can be passed the same signal more than once", () => {
  const c = new AbortController();
  const signal = AbortSignal.any([c.signal, c.signal]);
  let count = 0;
  signal.onabort = () => { count++; };
  c.abort("reason");
  same(signal.reason, "reason", "reason");
  same(count, 1, "one event");
});

test("AbortSignal.any() uses the first instance of a duplicate signal", () => {
  const c1 = new AbortController();
  c1.abort("reason 1");
  const c2 = new AbortController();
  c2.abort("reason 2");
  same(AbortSignal.any([c1.signal, c2.signal, c1.signal]).reason, "reason 1", "reason");
});

test("AbortSignal.any() signals are composable", () => {
  for (let i = 0; i < 3; ++i) {
    const controllers = [new AbortController(), new AbortController(), new AbortController()];
    const combined1 = AbortSignal.any([controllers[0].signal, controllers[1].signal]);
    const combined2 = AbortSignal.any([combined1, controllers[2].signal]);
    let fired = false;
    combined2.onabort = () => { fired = true; };
    controllers[i].abort();
    same(fired, true, `fired for ${i}`);
    same(combined2.reason.name, "AbortError", "AbortError");
  }
});

test("AbortSignal.any() works with intermediate signals", () => {
  const c = new AbortController();
  let combined = AbortSignal.any([c.signal]);
  combined = AbortSignal.any([combined]);
  combined = AbortSignal.any([combined]);
  combined = AbortSignal.any([combined]);
  let fired = false;
  combined.onabort = () => { fired = true; };
  c.abort("the reason");
  same(fired, true, "fired");
  same(combined.reason, "the reason", "reason");
});

test("Abort events for AbortSignal.any() signals fire in the right order", () => {
  const c = new AbortController();
  const signals = [c.signal];
  signals.push(AbortSignal.any([c.signal]));
  signals.push(AbortSignal.any([c.signal]));
  signals.push(AbortSignal.any([signals[0]]));
  signals.push(AbortSignal.any([signals[1]]));
  let result = "";
  for (let i = 0; i < signals.length; i++) {
    signals[i].addEventListener("abort", () => { result += i; });
  }
  c.abort();
  same(result, "01234", "order");
});

test("Dependent signals are marked aborted before abort events fire", () => {
  const c = new AbortController();
  const signal1 = AbortSignal.any([c.signal]);
  const signal2 = AbortSignal.any([signal1]);
  let fired = false;
  c.signal.addEventListener("abort", () => {
    const signal3 = AbortSignal.any([signal2]);
    same(signal1.aborted && signal2.aborted && signal3.aborted, true, "all aborted");
    fired = true;
  });
  c.abort();
  same(fired, true, "fired");
});

test("Dependent signals are aborted correctly for reentrant aborts", () => {
  const c1 = new AbortController();
  const c2 = new AbortController();
  const signal = AbortSignal.any([c1.signal, c2.signal]);
  let count = 0;
  c1.signal.addEventListener("abort", () => { c2.abort("reason 2"); });
  signal.addEventListener("abort", () => { count++; });
  c1.abort("reason 1");
  same(count, 1, "one event");
  same(signal.reason, "reason 1", "reason");
});

test("Dependent signals use the same DOMException instance as their source", () => {
  const source = AbortSignal.abort();
  same(AbortSignal.any([source]).reason, source.reason, "already aborted");
  const c = new AbortController();
  const dependent = AbortSignal.any([c.signal]);
  c.abort();
  same(dependent.reason, c.signal.reason, "aborted later");
});

test("AbortSignal.any() reads any iterable with its own iterator", () => {
  const c = new AbortController();
  const set = new Set([c.signal]);
  const fromSet = AbortSignal.any(set);
  function* gen() { yield c.signal; }
  const fromGenerator = AbortSignal.any(gen());
  c.abort("it");
  same(fromSet.reason, "it", "a Set");
  same(fromGenerator.reason, "it", "a generator");
});

test("AbortSignal.any() argument errors", () => {
  throwsType(TypeError, () => AbortSignal.any(), "no argument");
  throwsType(TypeError, () => AbortSignal.any(5), "a number");
  throwsType(TypeError, () => AbortSignal.any({}), "not iterable");
  throwsType(TypeError, () => AbortSignal.any([new AbortController().signal, {}]), "not a signal");
  throwsType(TypeError, () => AbortSignal.any([null]), "null");
  const err = new Error("from the iterator");
  try {
    AbortSignal.any({ [Symbol.iterator]() { throw err; } });
    throw new Error("did not throw");
  } catch (e) {
    same(e, err, "the iterator's own exception");
  }
});

test("a dependent signal can itself be a source of nothing but its sources", () => {
  // AbortSignal.any([dependent]) links to the dependent's sources, so
  // aborting through the dependent's source reaches both.
  const c = new AbortController();
  const d1 = AbortSignal.any([c.signal]);
  const d2 = AbortSignal.any([d1, d1]);
  let seen = "";
  d1.onabort = () => { seen += "1"; };
  d2.onabort = () => { seen += "2"; };
  c.abort();
  same(seen, "12", "order");
});

// ---------------------------------------------------------------------
// dom/abort/timeout.any.js and AbortSignal.timeout argument conversion
// ---------------------------------------------------------------------

test("AbortSignal.timeout() returns a non-aborted signal", () => {
  const signal = AbortSignal.timeout(0);
  assert(signal instanceof AbortSignal, "an AbortSignal");
  same(signal.aborted, false, "not aborted yet");
});

test("AbortSignal.timeout() argument conversion ([EnforceRange] unsigned long long)", () => {
  for (const bad of [NaN, Infinity, -Infinity, -1, 2 ** 53, undefined, "x"]) {
    throwsType(TypeError, () => AbortSignal.timeout(bad), `timeout(${String(bad)})`);
  }
  throwsType(TypeError, () => AbortSignal.timeout(), "no argument");
  throwsType(TypeError, () => AbortSignal.timeout(1n), "a BigInt");
  for (const good of [0, -0.5, 1.9, "5", 2 ** 53 - 1]) {
    same(AbortSignal.timeout(good).aborted, false, `timeout(${String(good)})`);
  }
});

asyncTest("Signal returned by AbortSignal.timeout() times out", async () => {
  const signal = AbortSignal.timeout(5);
  let event;
  signal.onabort = (e) => { event = e; };
  await delay(60);
  assert(event !== undefined, "fired");
  same(event.isTrusted, true, "trusted");
  same(signal.aborted, true, "aborted");
  assert(signal.reason instanceof DOMException, "a DOMException");
  same(signal.reason.name, "TimeoutError", "TimeoutError");
  same(signal.reason.message, "signal timed out", "message");
  same(signal.reason.code, 23, "code");
});

asyncTest("AbortSignal timeouts fire in order", async () => {
  let result = "";
  for (const value of ["1", "2", "3"]) {
    AbortSignal.timeout(5).onabort = () => { result += value; };
  }
  await delay(60);
  same(result, "123", "order");
});

asyncTest("AbortSignal.any() works with signals returned by AbortSignal.timeout()", async () => {
  const c = new AbortController();
  const combined = AbortSignal.any([c.signal, AbortSignal.timeout(5)]);
  let fired = false;
  combined.onabort = () => { fired = true; };
  await delay(60);
  same(fired, true, "fired");
  same(combined.reason.name, "TimeoutError", "TimeoutError");
});

asyncTest("clearTimeout cannot cancel AbortSignal.timeout's timer (Forge)", async () => {
  // Timer ids count up, and the internal timer takes the next one. (Only
  // ids after `id`: the other tests' timers have smaller ones.)
  const id = setTimeout(() => {}, 0);
  clearTimeout(id);
  const signal = AbortSignal.timeout(5);
  for (let i = id + 1; i < id + 20; i++) {
    clearTimeout(i);
  }
  await delay(60);
  same(signal.aborted, true, "still timed out");
});

asyncTest("a timeout signal removes a listener when it fires", async () => {
  const et = new EventTarget();
  let count = 0;
  et.addEventListener("x", () => { count++; }, { signal: AbortSignal.timeout(5) });
  et.dispatchEvent(new Event("x"));
  await delay(60);
  et.dispatchEvent(new Event("x"));
  same(count, 1, "removed by the timeout");
});

// ---------------------------------------------------------------------
// Robustness (Forge, PHASE10_DESIGN.md)
// ---------------------------------------------------------------------

test("changing the built-ins does not change AbortSignal", () => {
  const saved = {
    push: Array.prototype.push,
    iterator: Array.prototype[Symbol.iterator],
    proto: Object.getPrototypeOf(AbortSignal),
  };
  let called = false;
  Array.prototype.push = function () { called = true; throw new Error("push"); };
  Object.defineProperty(Object.prototype, "length", {
    set() { called = true; }, get() { return 0; }, configurable: true,
  });
  Object.setPrototypeOf(AbortSignal, function Evil() { called = true; });
  try {
    const c = new AbortController();
    const d = AbortSignal.any(new Set([c.signal]));
    const t = AbortSignal.timeout(100000);
    let seen = 0;
    d.addEventListener("abort", () => { seen++; });
    new EventTarget().addEventListener("x", () => {}, { signal: c.signal });
    c.abort();
    same(seen, 1, "dependent fired");
    same(t.aborted, false, "timeout signal made");
    same(called, false, "no user code ran");
  } finally {
    Array.prototype.push = saved.push;
    delete Object.prototype.length;
    Object.setPrototypeOf(AbortSignal, saved.proto);
  }
});

test("a changed ErrorEvent parent does not run when Forge reports an error (10.2 hardening)", () => {
  const saved = Object.getPrototypeOf(ErrorEvent);
  let called = false;
  let seen;
  const onError = (e) => { seen = e; e.preventDefault(); };
  Object.setPrototypeOf(ErrorEvent, function Evil() { called = true; });
  addEventListener("error", onError);
  try {
    reportError(new Error("hardening"));
  } finally {
    removeEventListener("error", onError);
    Object.setPrototypeOf(ErrorEvent, saved);
  }
  same(called, false, "no user code ran");
  assert(seen instanceof ErrorEvent && seen.isTrusted, "a trusted ErrorEvent was dispatched");
  same(Object.getPrototypeOf(ErrorEvent), saved, "parent restored");
});

test("a dependent signal with a listener but no reference left still fires", () => {
  const c = new AbortController();
  let fired = false;
  (() => {
    AbortSignal.any([c.signal]).addEventListener("abort", () => { fired = true; });
  })();
  c.abort();
  same(fired, true, "fired");
});

Promise.all(pending).then(() => {
  console.log(`abort done: ${checks} checks, ${failures} failed`);
  if (failures > 0) {
    Forge.exitCode = 1;
  }
});
