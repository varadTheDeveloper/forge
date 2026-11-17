// Phase 10.2: Event, CustomEvent, ErrorEvent, PromiseRejectionEvent,
// EventTarget, the global object as an EventTarget, its event handler
// attributes, reportError, self and navigator.
//
// The checks follow the Web Platform Tests for these APIs (dom/events/,
// html/webappapis/scripting/reporterror.any.js), transcribed by hand, plus
// Forge's own rules (PHASE10_DESIGN.md). Every failure prints a line
// starting with "EVENTS FAIL" and makes the exit code 1; the last line is
// "events done: <checks> checks, <failures> failed". Errors reported here
// are all cancelled by a listener, so none is fatal.

"use strict";

let checks = 0;
let failures = 0;

function test(name, body) {
  checks++;
  try {
    body();
  } catch (e) {
    failures++;
    console.log(`EVENTS FAIL ${name}: ${e && e.message ? e.message : String(e)}`);
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

// ---------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------

for (const name of ["Event", "CustomEvent", "ErrorEvent", "PromiseRejectionEvent", "EventTarget"]) {
  test(`${name} global property`, () => {
    const d = Object.getOwnPropertyDescriptor(globalThis, name);
    assert(d && typeof d.value === "function", "exists");
    assert(d.writable && !d.enumerable && d.configurable, "attributes");
    same(Object.prototype.toString.call(d.value.prototype), `[object ${name}]`, "toStringTag");
    throwsType(TypeError, () => d.value("x"), "needs new");
  });
}

test("inheritance", () => {
  same(Object.getPrototypeOf(Event), Function.prototype, "Event");
  same(Object.getPrototypeOf(EventTarget), Function.prototype, "EventTarget");
  same(Object.getPrototypeOf(CustomEvent), Event, "CustomEvent");
  same(Object.getPrototypeOf(ErrorEvent.prototype), Event.prototype, "ErrorEvent.prototype");
  same(Object.getPrototypeOf(PromiseRejectionEvent.prototype), Event.prototype, "PromiseRejectionEvent.prototype");
  same(Event.length, 1, "Event.length");
  same(PromiseRejectionEvent.length, 2, "PromiseRejectionEvent.length");
  same(EventTarget.length, 0, "EventTarget.length");
});

// ---------------------------------------------------------------------
// Event (dom/events/Event-constructors.any.js and friends)
// ---------------------------------------------------------------------

test("Event: constructor arguments", () => {
  throwsType(TypeError, () => new Event(), "no type");
  const thrown = { name: "test" };
  try {
    new Event({ toString() { throw thrown; } });
    throw new Error("did not throw");
  } catch (e) {
    same(e, thrown, "ToString of the type rethrows");
  }
  throwsType(TypeError, () => new Event("x", 5), "dictionary not an object");
});

test("Event: defaults", () => {
  for (const type of ["", "test"]) {
    const ev = new Event(type);
    same(ev.type, type, "type");
    same(ev.target, null, "target");
    same(ev.srcElement, null, "srcElement");
    same(ev.currentTarget, null, "currentTarget");
    same(ev.eventPhase, Event.NONE, "eventPhase");
    same(ev.bubbles, false, "bubbles");
    same(ev.cancelable, false, "cancelable");
    same(ev.defaultPrevented, false, "defaultPrevented");
    same(ev.returnValue, true, "returnValue");
    same(ev.isTrusted, false, "isTrusted");
    same(ev.composed, false, "composed");
    assert(typeof ev.timeStamp === "number" && ev.timeStamp > 0, "timeStamp > 0");
    assert("initEvent" in ev, "initEvent");
    same(ev.composedPath().length, 0, "composedPath");
  }
});

test("Event: dictionary members and their order", () => {
  const called = [];
  const ev = new Event("Xx", {
    get cancelable() { called.push("cancelable"); return false; },
    get bubbles() { called.push("bubbles"); return true; },
    get composed() { called.push("composed"); return true; },
    get sweet() { called.push("sweet"); return "x"; },
  });
  same(called.join(), "bubbles,cancelable,composed", "order");
  same(ev.bubbles, true, "bubbles");
  same(ev.composed, true, "composed");
  same(ev.sweet, undefined, "unknown member ignored");
  same(new Event("@", { "bubbles\0IGNORED": true, cancelable: true }).bubbles, false, "near-miss name");
});

test("Event: constants", () => {
  const names = ["NONE", "CAPTURING_PHASE", "AT_TARGET", "BUBBLING_PHASE"];
  names.forEach((name, i) => {
    for (const object of [Event, Event.prototype]) {
      const d = Object.getOwnPropertyDescriptor(object, name);
      same(d.value, i, name);
      assert(!d.writable && d.enumerable && !d.configurable, name + " attributes");
    }
  });
});

test("Event: isTrusted is an unforgeable own accessor", () => {
  const d1 = Object.getOwnPropertyDescriptor(new Event("x"), "isTrusted");
  const d2 = Object.getOwnPropertyDescriptor(new Event("x"), "isTrusted");
  assert(d1 && typeof d1.get === "function", "own getter");
  same(d1.get, d2.get, "the same getter");
  assert(d1.enumerable && !d1.configurable && d1.set === undefined, "attributes");
  throwsType(TypeError, () => d1.get.call({}), "brand check");
});

test("Event: returnValue and preventDefault", () => {
  let ev = new Event("foo", { cancelable: false });
  ev.preventDefault();
  same(ev.returnValue, true, "not cancelable: preventDefault");
  ev.returnValue = false;
  same(ev.returnValue, true, "not cancelable: returnValue = false");
  ev = new Event("foo", { cancelable: true });
  ev.preventDefault();
  same(ev.defaultPrevented, true, "cancelable: preventDefault");
  same(ev.returnValue, false, "returnValue after preventDefault");
  ev.returnValue = true;
  same(ev.defaultPrevented, true, "returnValue = true is a no-op");
  ev = new Event("foo", { cancelable: true });
  ev.returnValue = false;
  same(ev.defaultPrevented, true, "returnValue = false cancels");
});

test("Event: cancelBubble", () => {
  const ev = new Event("foo");
  same(ev.cancelBubble, false, "initially");
  ev.stopPropagation();
  same(ev.cancelBubble, true, "after stopPropagation");
  ev.cancelBubble = false;
  same(ev.cancelBubble, true, "setting false is a no-op");
  const ev2 = new Event("foo");
  ev2.stopImmediatePropagation();
  same(ev2.cancelBubble, true, "after stopImmediatePropagation");
  const ev3 = new Event("foo");
  ev3.cancelBubble = true;
  same(ev3.cancelBubble, true, "setting true stops propagation");
});

test("Event: initEvent", () => {
  const e = new Event("type 1", { bubbles: true });
  e.stopPropagation();
  e.initEvent("type 2", false, true);
  same(e.type, "type 2", "type");
  same(e.bubbles, false, "bubbles");
  same(e.cancelable, true, "cancelable");
  same(e.cancelBubble, false, "unsets stop propagation");
  e.initEvent("type 3");
  same(e.cancelable, false, "defaults");
  throwsType(TypeError, () => e.initEvent(), "type is required");
  const target = new EventTarget();
  target.addEventListener("type 3", () => {
    e.initEvent("fail", true, true);
    same(e.type, "type 3", "no effect while dispatching");
  });
  same(target.dispatchEvent(e), true, "dispatch");
});

test("Event: members and brand checks", () => {
  for (const key of ["type", "target", "currentTarget", "eventPhase", "bubbles", "cancelable",
                     "defaultPrevented", "timeStamp", "stopPropagation", "preventDefault",
                     "composedPath", "cancelBubble", "returnValue", "initEvent"]) {
    const d = Object.getOwnPropertyDescriptor(Event.prototype, key);
    assert(d && d.enumerable && d.configurable, key + " enumerable");
  }
  throwsType(TypeError, () => Event.prototype.preventDefault.call({}), "preventDefault");
  throwsType(TypeError, () => Object.getOwnPropertyDescriptor(Event.prototype, "type").get.call(new EventTarget()),
             "type on an EventTarget");
});

// ---------------------------------------------------------------------
// Subclasses
// ---------------------------------------------------------------------

test("CustomEvent", () => {
  const ev = new CustomEvent("$", { detail: 54, sweet: "x", cancelable: true });
  same(ev.type, "$", "type");
  same(ev.cancelable, true, "cancelable");
  same(ev.detail, 54, "detail");
  same(new CustomEvent("x").detail, null, "detail defaults to null");
  const order = [];
  new CustomEvent("x", {
    get detail() { order.push("detail"); return 1; },
    get bubbles() { order.push("bubbles"); return false; },
  });
  same(order.join(), "bubbles,detail", "Event's members first");
  ev.initCustomEvent("y", true, false, "d");
  same(ev.type, "y", "initCustomEvent type");
  same(ev.detail, "d", "initCustomEvent detail");
  assert(ev instanceof Event, "instanceof Event");
});

test("ErrorEvent", () => {
  const error = new Error("boom");
  const order = [];
  const tracked = (name, value) => ({ get() { order.push(name); return value; } });
  const init = Object.defineProperties({}, {
    message: tracked("message", "m"), filename: tracked("filename", "f.js"),
    lineno: tracked("lineno", 3), colno: tracked("colno", 4), error: tracked("error", error),
  });
  const ev = new ErrorEvent("error", init);
  same(order.join(), "colno,error,filename,lineno,message", "lexicographic order");
  same(ev.message, "m", "message");
  same(ev.filename, "f.js", "filename");
  same(ev.lineno, 3, "lineno");
  same(ev.colno, 4, "colno");
  same(ev.error, error, "error");
  const d = new ErrorEvent("error");
  same(d.message, "", "default message");
  same(d.lineno, 0, "default lineno");
  same(d.error, undefined, "default error");
  same(new ErrorEvent("e", { lineno: -1 }).lineno, 4294967295, "unsigned long wraps");
  same(new ErrorEvent("e", { colno: NaN }).colno, 0, "NaN is 0");
  same(new ErrorEvent("e", { filename: "a\uD800" }).filename, "a\uFFFD", "USVString");
  throwsType(TypeError, () => new ErrorEvent("e", { lineno: 1n }), "BigInt is a TypeError");
});

test("PromiseRejectionEvent", () => {
  const p = Promise.resolve(1);
  const ev = new PromiseRejectionEvent("unhandledrejection", { promise: p, reason: "r", cancelable: true });
  same(ev.promise, p, "promise kept as is");
  same(ev.reason, "r", "reason");
  same(ev.cancelable, true, "cancelable");
  same(new PromiseRejectionEvent("x", { promise: p }).reason, undefined, "reason defaults to undefined");
  const plain = {};
  same(new PromiseRejectionEvent("x", { promise: plain }).promise, plain, "any object");
  throwsType(TypeError, () => new PromiseRejectionEvent("x"), "dictionary is required");
  throwsType(TypeError, () => new PromiseRejectionEvent("x", {}), "promise is required");
  throwsType(TypeError, () => new PromiseRejectionEvent("x", { promise: 1 }), "promise must be an object");
});

// ---------------------------------------------------------------------
// EventTarget (dom/events/EventTarget-constructible.any.js and friends)
// ---------------------------------------------------------------------

test("EventTarget: constructed targets work", () => {
  const target = new EventTarget();
  const event = new Event("foo", { bubbles: true });
  let calls = 0;
  function listener(e) {
    same(e, event, "the event");
    same(e.target, target, "target");
    same(e.currentTarget, target, "currentTarget");
    same(e.eventPhase, Event.AT_TARGET, "eventPhase");
    same(this, target, "this is the target");
    same(e.composedPath().length, 1, "composedPath");
    same(e.composedPath()[0], target, "composedPath[0]");
    calls++;
  }
  target.addEventListener("foo", listener);
  same(target.dispatchEvent(event), true, "returns true");
  target.dispatchEvent(event);
  same(calls, 2, "called twice");
  same(event.target, target, "target stays");
  same(event.currentTarget, null, "currentTarget cleared");
  same(event.eventPhase, Event.NONE, "phase cleared");
  same(event.composedPath().length, 0, "path cleared");
  target.removeEventListener("foo", listener);
  target.dispatchEvent(event);
  same(calls, 2, "removed");
});

test("EventTarget: subclassing", () => {
  class Nicer extends EventTarget {
    on(...args) { this.addEventListener(...args); }
    dispatch(type, detail) { return this.dispatchEvent(new CustomEvent(type, { detail })); }
  }
  const target = new Nicer();
  let seen;
  target.on("foo", (e) => { seen = e.detail; });
  target.dispatch("foo", "data");
  same(seen, "data", "detail");
  assert(target instanceof EventTarget, "instanceof");
});

test("EventTarget: duplicates, capture and order", () => {
  const target = new EventTarget();
  const order = [];
  const a = () => order.push("a");
  const b = () => order.push("b");
  const c = () => order.push("capture");
  target.addEventListener("x", a);
  target.addEventListener("x", a);
  target.addEventListener("x", b, false);
  target.addEventListener("x", c, true);
  target.addEventListener("x", c, { capture: true });
  target.dispatchEvent(new Event("x"));
  same(order.join(), "capture,a,b", "capture first, no duplicates");
  target.removeEventListener("x", c);
  order.length = 0;
  target.dispatchEvent(new Event("x"));
  same(order.join(), "capture,a,b", "remove without capture leaves the capturing one");
  target.removeEventListener("x", c, { capture: true });
  order.length = 0;
  target.dispatchEvent(new Event("x"));
  same(order.join(), "a,b", "removed with capture");
});

test("EventTarget: once (AddEventListenerOptions-once)", () => {
  const et = new EventTarget();
  let once = 0;
  let normal = 0;
  et.addEventListener("test", () => once++, { once: true });
  et.addEventListener("test", () => normal++);
  et.dispatchEvent(new Event("test"));
  et.dispatchEvent(new Event("test"));
  same(once, 1, "once listener");
  same(normal, 2, "normal listener");
  let count = 0;
  function handler() {
    count++;
    if (count === 1) {
      et.dispatchEvent(new Event("nested"));
    }
  }
  et.addEventListener("nested", handler, { once: true });
  et.dispatchEvent(new Event("nested"));
  same(count, 1, "nested dispatch does not call it again");
  let n = 0;
  const h = () => n++;
  et.addEventListener("t2", h, { once: true });
  et.addEventListener("t2", h);
  et.dispatchEvent(new Event("t2"));
  et.dispatchEvent(new Event("t2"));
  same(n, 1, "added once, as a once listener");
  let stopped = 0;
  for (let i = 0; i < 4; i++) {
    et.addEventListener("t3", (e) => { stopped++; e.stopImmediatePropagation(); }, { once: true });
  }
  for (let i = 0; i < 4; i++) {
    et.dispatchEvent(new Event("t3"));
  }
  same(stopped, 4, "stopImmediatePropagation with once listeners");
});

test("EventTarget: passive listeners cannot cancel", () => {
  const et = new EventTarget();
  et.addEventListener("x", (e) => e.preventDefault(), { passive: true });
  const ev = new Event("x", { cancelable: true });
  same(et.dispatchEvent(ev), true, "not cancelled");
  same(ev.defaultPrevented, false, "defaultPrevented");
  et.addEventListener("x", (e) => e.preventDefault());
  same(et.dispatchEvent(new Event("x", { cancelable: true })), false, "a non-passive one can");
});

test("EventTarget: stopImmediatePropagation and listeners added during dispatch", () => {
  const et = new EventTarget();
  const order = [];
  et.addEventListener("x", (e) => { order.push(1); et.addEventListener("x", () => order.push("late")); });
  et.addEventListener("x", (e) => { order.push(2); e.stopImmediatePropagation(); });
  et.addEventListener("x", () => order.push(3));
  et.dispatchEvent(new Event("x"));
  same(order.join(), "1,2", "stopped, and the late listener waits for the next dispatch");
  const ev = new Event("y");
  et.addEventListener("y", (e) => e.stopPropagation());
  et.addEventListener("y", () => order.push("y2"));
  et.dispatchEvent(ev);
  assert(order.includes("y2"), "stopPropagation does not stop listeners on the same target");
  same(ev.cancelBubble, false, "flags reset after dispatch");
});

test("EventTarget: removal during dispatch", () => {
  const et = new EventTarget();
  let second = 0;
  const b = () => second++;
  et.addEventListener("x", () => et.removeEventListener("x", b));
  et.addEventListener("x", b);
  et.dispatchEvent(new Event("x"));
  same(second, 0, "a listener removed during dispatch is not called");
});

test("EventTarget: handleEvent objects", () => {
  const et = new EventTarget();
  let gets = 0;
  let thisValue;
  const listener = {
    get handleEvent() {
      gets++;
      return function () { thisValue = this; };
    },
  };
  et.addEventListener("x", listener);
  same(gets, 0, "not looked up when added");
  et.dispatchEvent(new Event("x"));
  et.dispatchEvent(new Event("x"));
  same(gets, 2, "looked up at each dispatch");
  same(thisValue, listener, "this is the object");
  const fn = () => {};
  let fnHandle = 0;
  fn.handleEvent = () => fnHandle++;
  et.addEventListener("y", fn);
  et.dispatchEvent(new Event("y"));
  same(fnHandle, 0, "a function's handleEvent is not used");
});

test("EventTarget: argument checks", () => {
  const et = new EventTarget();
  throwsType(TypeError, () => et.addEventListener("x"), "callback is required");
  throwsType(TypeError, () => et.addEventListener("x", 5), "callback must be an object");
  et.addEventListener("x", null);
  et.addEventListener("x", undefined);
  throwsType(TypeError, () => et.dispatchEvent({ type: "x" }), "not an Event");
  throwsType(TypeError, () => et.dispatchEvent(), "event is required");
  throwsType(TypeError, () => et.addEventListener("x", () => {}, { signal: {} }), "not an AbortSignal");
  throwsType(TypeError, () => EventTarget.prototype.addEventListener.call({}, "x", () => {}), "brand check");
  const order = [];
  et.addEventListener({ toString() { order.push("type"); return "z"; } }, () => {},
                      { get capture() { order.push("capture"); return false; },
                        get once() { order.push("once"); return false; },
                        get passive() { order.push("passive"); return false; } });
  same(order.join(), "type,capture,once,passive", "conversion order");
});

test("EventTarget: dispatching an event twice at once", () => {
  const et = new EventTarget();
  const ev = new Event("x");
  let inner;
  et.addEventListener("x", () => {
    try { et.dispatchEvent(ev); } catch (e) { inner = e; }
  });
  et.dispatchEvent(ev);
  assert(inner instanceof DOMException && inner.name === "InvalidStateError", "InvalidStateError");
});

test("EventTarget: isTrusted and the return value", () => {
  const et = new EventTarget();
  et.addEventListener("x", (e) => e.preventDefault());
  const ev = new Event("x", { cancelable: true });
  same(et.dispatchEvent(ev), false, "cancelled: false");
  same(ev.isTrusted, false, "dispatchEvent is never trusted");
});

// ---------------------------------------------------------------------
// The global object (html/webappapis, dom/events/event-global.worker.js)
// ---------------------------------------------------------------------

test("global: an EventTarget", () => {
  assert(globalThis instanceof EventTarget, "instanceof");
  same(Object.getPrototypeOf(globalThis), EventTarget.prototype, "prototype");
  let seen = 0;
  const ev = new Event("hi");
  let first;
  const listener = (e) => { seen++; first = first || e; same(e.currentTarget, globalThis, "currentTarget"); };
  addEventListener("hi", listener);
  dispatchEvent(ev);
  same(first, ev, "the event");
  globalThis.dispatchEvent(new Event("hi"));
  removeEventListener("hi", listener);
  dispatchEvent(new Event("hi"));
  same(seen, 2, "bare and qualified calls");
  same(globalThis.event, undefined, "no window.event");
});

test("global: self and navigator", () => {
  same(self, globalThis, "self");
  const d = Object.getOwnPropertyDescriptor(globalThis, "self");
  assert(typeof d.get === "function" && typeof d.set === "function" && d.enumerable && d.configurable,
         "self is a replaceable accessor");
  same(navigator.userAgent, "Forge", "userAgent");
  same(navigator, navigator, "same object");
  assert(Object.isFrozen(navigator), "navigator is frozen");
  same(typeof globalThis.Navigator, "undefined", "Navigator is not a global");
  throwsType(TypeError, () => new (Object.getPrototypeOf(navigator).constructor)(), "illegal constructor");
  throwsType(TypeError, () => Object.getOwnPropertyDescriptor(globalThis, "navigator").get.call({}),
             "not the global object");
});

test("global: event handler attributes", () => {
  for (const name of ["onerror", "onunhandledrejection", "onrejectionhandled"]) {
    const d = Object.getOwnPropertyDescriptor(globalThis, name);
    assert(d && d.get && d.set && d.enumerable && d.configurable, name + " accessor");
    same(globalThis[name], null, name + " starts null");
  }
  const f = () => {};
  onerror = f;
  same(onerror, f, "stored");
  onerror = 5;
  same(onerror, null, "a non-object is null");
  onerror = null;
});

test("global: event handlers run in order, and onerror gets five arguments", () => {
  const order = [];
  addEventListener("error", () => order.push("first"));
  onerror = (message, source, lineno, colno, error) => {
    order.push(`handler ${message} ${source} ${lineno} ${colno} ${error}`);
    return true;
  };
  addEventListener("error", () => order.push("last"));
  const ev = new ErrorEvent("error", { message: "m", filename: "f", lineno: 1, colno: 2, error: "e", cancelable: true });
  same(dispatchEvent(ev), false, "return true cancels");
  same(order.join("|"), "first|handler m f 1 2 e|last", "order and arguments");
  onerror = () => {};
  order.length = 0;
  dispatchEvent(new ErrorEvent("error", { cancelable: true }));
  same(order.join("|"), "first|last", "replacing keeps the handler's place");
  onerror = null;
  onunhandledrejection = () => false;
  const pre = new PromiseRejectionEvent("unhandledrejection", { promise: {}, cancelable: true });
  same(dispatchEvent(pre), false, "an ordinary handler cancels by returning false");
  onunhandledrejection = null;
});

// ---------------------------------------------------------------------
// reportError (html/webappapis/scripting/reporterror.any.js)
// ---------------------------------------------------------------------

test("reportError", () => {
  for (const throwable of [1, new TypeError(), undefined]) {
    let happened = false;
    addEventListener("error", (e) => {
      assert(e instanceof ErrorEvent, "an ErrorEvent");
      assert(e.message !== "", "message");
      assert(e.lineno > 0, "lineno");
      assert(e.colno > 0, "colno");
      same(e.error, throwable, "error");
      same(e.isTrusted, true, "trusted");
      same(e.cancelable, true, "cancelable");
      happened = true;
      e.preventDefault();
    }, { once: true });
    reportError(throwable);
    assert(happened, "the error event was dispatched");
  }
  throwsType(TypeError, () => reportError(), "an argument is required");
  let invoked = false;
  addEventListener("error", (e) => e.preventDefault(), { once: true });
  reportError({
    get name() { invoked = true; return "x"; },
    get message() { invoked = true; return "x"; },
    get fileName() { invoked = true; return "x"; },
    get lineNumber() { invoked = true; return 1; },
  });
  same(invoked, false, "no getter is called");
});

test("a listener's exception is reported and dispatch goes on", () => {
  const et = new EventTarget();
  let reported;
  let after = false;
  addEventListener("error", (e) => { reported = e.error; e.preventDefault(); }, { once: true });
  const boom = new Error("listener boom");
  et.addEventListener("x", () => { throw boom; });
  et.addEventListener("x", () => { after = true; });
  same(et.dispatchEvent(new Event("x")), true, "dispatchEvent returns normally");
  same(reported, boom, "reported to the global");
  same(after, true, "later listeners still run");
  let typeErr;
  addEventListener("error", (e) => { typeErr = e.error; e.preventDefault(); }, { once: true });
  et.addEventListener("y", { handleEvent: 42 });
  et.dispatchEvent(new Event("y"));
  assert(typeErr instanceof TypeError, "a non-callable handleEvent is a reported TypeError");
});

// ---------------------------------------------------------------------
// Primordials (rule 1 of runtime/WebApis.js)
// ---------------------------------------------------------------------

test("primordials: patched built-ins are not used", () => {
  const saved = {
    get: Map.prototype.get, set: Map.prototype.set, wget: WeakMap.prototype.get,
    apply: Reflect.apply, iterator: Array.prototype[Symbol.iterator],
  };
  let calls = 0;
  const et = new EventTarget();
  let ran = 0;
  try {
    Map.prototype.get = function () { calls++; return undefined; };
    Map.prototype.set = function () { calls++; return this; };
    WeakMap.prototype.get = function () { calls++; return undefined; };
    Reflect.apply = function () { calls++; throw new Error("patched apply"); };
    Array.prototype[Symbol.iterator] = function () { calls++; throw new Error("patched iterator"); };
    Object.defineProperty(Object.prototype, "0", { set() { calls++; }, configurable: true });
    Object.defineProperty(Object.prototype, "length", { set() { calls++; }, configurable: true });
    et.addEventListener("x", () => ran++);
    et.dispatchEvent(new Event("x"));
  } finally {
    Map.prototype.get = saved.get;
    Map.prototype.set = saved.set;
    WeakMap.prototype.get = saved.wget;
    Reflect.apply = saved.apply;
    Array.prototype[Symbol.iterator] = saved.iterator;
    delete Object.prototype[0];
    delete Object.prototype.length;
  }
  same(ran, 1, "the listener ran");
  same(calls, 0, "patched functions called");
});

console.log(`events done: ${checks} checks, ${failures} failed`);
if (failures !== 0) {
  Forge.exitCode = 1;
}
