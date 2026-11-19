// Phase 10.4: MessageChannel, MessagePort and MessageEvent.
//
// The checks follow the Web Platform Tests for these APIs (webmessaging/,
// webmessaging/message-channels/), transcribed by hand under the WPT
// names where there is one, so tests/wpt/coverage.json can map each
// (decision D29); Forge's own checks follow. Cases transcribed from WPT are
// under WPT's 3-Clause BSD License (tests/wpt/LICENSE.md).
//
// Every failure prints a line starting with "MESSAGING FAIL"; the last line
// is "messaging done: <checks> checks, <failures> failed".

"use strict";

let checks = 0;
let failures = 0;
const pending = [];

function fail(name, e) {
  failures++;
  console.log(`MESSAGING FAIL ${name}: ${e && e.message ? e.message : String(e)}`);
}

function test(name, body) {
  checks++;
  try {
    body();
  } catch (e) {
    fail(name, e);
  }
}

// An asynchronous check: `body(done)` calls done() once it has passed;
// throwing (or a step throwing) fails it. It also fails if done() was not
// called within `ms` milliseconds.
function asyncTest(name, body, ms = 300) {
  checks++;
  pending.push(new Promise((resolve) => {
    let finished = false;
    const finish = (error) => {
      if (!finished) {
        finished = true;
        if (error !== undefined) {
          fail(name, error);
        }
        resolve();
      }
    };
    const step = (f) => (...args) => {
      try {
        return f(...args);
      } catch (e) {
        finish(e);
      }
    };
    setTimeout(() => finish(new Error("timed out")), ms);
    try {
      body(() => finish(undefined), step, (what) => finish(new Error(what)));
    } catch (e) {
      finish(e);
    }
  }));
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

// How long the "nothing arrives" checks wait (WPT waits 50 to 100 ms).
const quiet = 60;

// ---------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------

for (const name of ["MessageChannel", "MessagePort", "MessageEvent"]) {
  test(`${name} global property`, () => {
    const d = Object.getOwnPropertyDescriptor(globalThis, name);
    assert(d && typeof d.value === "function", "exists");
    assert(d.writable && !d.enumerable && d.configurable, "attributes");
    same(Object.prototype.toString.call(d.value.prototype), `[object ${name}]`, "toStringTag");
  });
}

test("prototype chains and members", () => {
  same(Object.getPrototypeOf(MessagePort), EventTarget, "MessagePort interface object");
  same(Object.getPrototypeOf(MessagePort.prototype), EventTarget.prototype, "MessagePort prototype");
  same(Object.getPrototypeOf(MessageEvent.prototype), Event.prototype, "MessageEvent prototype");
  same(Object.keys(MessagePort.prototype).join(), "postMessage,start,close,onmessage,onmessageerror",
       "MessagePort members");
  same(Object.keys(MessageChannel.prototype).join(), "port1,port2", "MessageChannel members");
  same(Object.keys(MessageEvent.prototype).join(), "data,origin,lastEventId,source,ports,initMessageEvent",
       "MessageEvent members");
  same(MessageChannel.length, 0, "MessageChannel.length");
  same(MessageEvent.length, 1, "MessageEvent.length");
  same(MessagePort.prototype.postMessage.length, 1, "postMessage.length");
  same(MessageEvent.prototype.initMessageEvent.length, 1, "initMessageEvent.length");
});

test("MessagePort has no constructor", () => {
  throwsType(TypeError, () => new MessagePort(), "new MessagePort()");
});

test("brand checks", () => {
  const notPort = new EventTarget();
  throwsType(TypeError, () => MessagePort.prototype.postMessage.call(notPort, 1), "postMessage");
  throwsType(TypeError, () => MessagePort.prototype.start.call({}), "start");
  throwsType(TypeError, () => MessagePort.prototype.close.call({}), "close");
  const port1 = Object.getOwnPropertyDescriptor(MessageChannel.prototype, "port1").get;
  throwsType(TypeError, () => port1.call({}), "port1");
  const data = Object.getOwnPropertyDescriptor(MessageEvent.prototype, "data").get;
  throwsType(TypeError, () => data.call(new Event("x")), "data");
});

test("MessagePort should not have an onclose event handler IDL attribute", () => {
  assert(!("onclose" in MessagePort.prototype), "MessagePort.prototype");
  assert(!("onclose" in new MessageChannel().port1), "a MessagePort");
});

// ---------------------------------------------------------------------
// MessageEvent (webmessaging/MessageEvent.any.js and more)
// ---------------------------------------------------------------------

// [method, name of the prototype check, name of the instance check]
for (const [method, onPrototype, onInstance] of [
  ["mozInitMessageEvent", "mozInitMessageEvent on the prototype", "mozInitMessageEvent on the instance"],
  ["msInitMessageEvent", "msInitMessageEvent on the prototype", "msInitMessageEvent on the instance"],
  ["oInitMessageEvent", "oInitMessageEvent on the prototype", "oInitMessageEvent on the instance"],
  ["webkitInitMessageEvent", "webkitInitMessageEvent on the prototype", "webkitInitMessageEvent on the instance"],
]) {
  test(onPrototype, () => assert(!(method in MessageEvent.prototype), "absent"));
  test(onInstance, () => assert(!(method in new MessageEvent("message")), "absent"));
}

test("initMessageEvent with no arguments", () => {
  throwsType(TypeError, () => new MessageEvent("message").initMessageEvent(), "no arguments");
});

test("MessageEvent constructor defaults", () => {
  const e = new MessageEvent("message");
  same(e.type, "message", "type");
  same(e.data, null, "data");
  same(e.origin, "", "origin");
  same(e.lastEventId, "", "lastEventId");
  same(e.source, null, "source");
  assert(Array.isArray(e.ports) && e.ports.length === 0 && Object.isFrozen(e.ports), "ports");
  same(e.ports, e.ports, "the same ports array");
  same(e.isTrusted, false, "isTrusted");
  throwsType(TypeError, () => new MessageEvent(), "no type");
});

test("MessageEvent constructor reads its dictionary in order", () => {
  const read = [];
  const init = {};
  for (const name of ["source", "ports", "origin", "lastEventId", "data", "composed", "cancelable",
                      "bubbles"]) {
    Object.defineProperty(init, name, { get() { read.push(name); return undefined; } });
  }
  new MessageEvent("x", init);
  same(read.join(), "bubbles,cancelable,composed,data,lastEventId,origin,ports,source", "order");
});

test("MessageEvent constructor members", () => {
  const { port1, port2 } = new MessageChannel();
  const data = { a: 1 };
  const e = new MessageEvent("x", { data, origin: "o\uD800", lastEventId: 5, source: port1,
                                    ports: new Set([port2]) });
  same(e.data, data, "data is not cloned");
  same(e.origin, "o�", "origin is a USVString");
  same(e.lastEventId, "5", "lastEventId is a string");
  same(e.source, port1, "source");
  same(e.ports.length, 1, "ports");
  same(e.ports[0], port2, "ports[0]");
  assert(Object.isFrozen(e.ports), "ports frozen");
  throwsType(TypeError, () => new MessageEvent("x", { source: {} }), "source not a port");
  throwsType(TypeError, () => new MessageEvent("x", { ports: [{}] }), "ports item not a port");
  throwsType(TypeError, () => new MessageEvent("x", { ports: 5 }), "ports not a sequence");
});

test("initMessageEvent", () => {
  const { port1 } = new MessageChannel();
  const e = new MessageEvent("a");
  e.initMessageEvent("b", true, true, "d", "o", "id", port1, [port1]);
  same(e.type, "b", "type");
  same(e.bubbles, true, "bubbles");
  same(e.data, "d", "data");
  same(e.origin, "o", "origin");
  same(e.lastEventId, "id", "lastEventId");
  same(e.source, port1, "source");
  same(e.ports[0], port1, "ports");
  const target = new EventTarget();
  target.addEventListener("b", () => e.initMessageEvent("c", false, false, "changed"));
  target.dispatchEvent(e);
  same(e.data, "d", "unchanged while dispatching");
});

// ---------------------------------------------------------------------
// Ports: delivery (webmessaging/, webmessaging/message-channels/)
// ---------------------------------------------------------------------

asyncTest("basic messagechannel test", (done, step) => {
  const channel = new MessageChannel();
  channel.port1.postMessage(1);
  channel.port2.onmessage = step((e) => {
    same(e.data, 1, "data");
    done();
  });
  channel.port2.start();
});

asyncTest("basic messagechannel with transfer", (done, step) => {
  const channel = new MessageChannel();
  const ab = new ArrayBuffer(1);
  channel.port1.postMessage(ab, { transfer: [ab] });
  channel.port2.onmessage = step((e) => {
    same(e.data.byteLength, 1, "byteLength");
    same(ab.byteLength, 0, "the original is detached");
    done();
  });
  channel.port2.start();
});

asyncTest("onmessage implied start()", (done, step, failWith) => {
  const channel = new MessageChannel();
  channel.port1.postMessage(1);
  channel.port2.onmessage = () => {
    setTimeout(step(() => done()), 50);
    channel.port2.onmessage = () => failWith("a second message arrived");
  };
});

asyncTest("without start()", (done, step) => {
  const channel = new MessageChannel();
  channel.port1.postMessage(1);
  let i = 0;
  channel.port2.addEventListener("message", () => { i++; }, false);
  setTimeout(step(() => {
    same(i, 0, "no message");
    done();
  }), quiet);
});

asyncTest("MessageChannel: port message queue is initially disabled", (done, step, failWith) => {
  const channel = new MessageChannel();
  channel.port2.addEventListener("message", () => failWith("delivered"), true);
  channel.port1.postMessage("ping");
  setTimeout(() => done(), quiet);
});

asyncTest("MessageChannel: port.onmessage enables message queue", (done) => {
  const channel = new MessageChannel();
  channel.port2.onmessage = () => done();
  channel.port1.postMessage("ping");
});

asyncTest("The postMessage() method - Create an event that uses the MessageEvent interface, with the name message, which does not bubble and is not cancelable.", (done, step) => {
  const channel = new MessageChannel();
  channel.port2.start();
  channel.port2.addEventListener("message", step((evt) => {
    same(evt instanceof MessageEvent, true, "a MessageEvent");
    same(evt.type, "message", "type");
    same(evt.bubbles, false, "bubbles");
    same(evt.cancelable, false, "cancelable");
    same(evt.isTrusted, true, "isTrusted");
    same(evt.origin, "", "origin");
    same(evt.lastEventId, "", "lastEventId");
    same(evt.source, null, "source");
    done();
  }), true);
  channel.port1.postMessage("ping");
});

asyncTest("With a MessageChannel and its MessagePorts", (done, step) => {
  const channel = new MessageChannel();
  channel.port2.onmessage = step((e) => {
    same(e.isTrusted, true, "isTrusted");
    done();
  });
  channel.port1.postMessage("ping");
});

asyncTest("The postMessage() method - Let target port be the port with which source port is entangled, if any.", (done, step) => {
  const channel = new MessageChannel();
  const source = channel.port1;
  const target = channel.port2;
  target.start();
  target.addEventListener("message", step((evt) => {
    same(evt.target, target, "target");
    assert(evt.target !== source, "not the source");
    done();
  }), true);
  source.postMessage("ping");
});

asyncTest("The postMessage() method - Make new ports into a read only array.", (done, step) => {
  const channel = new MessageChannel();
  channel.port2.start();
  channel.port2.addEventListener("message", step((evt) => {
    const channel3 = new MessageChannel();
    throwsType(TypeError, () => evt.ports.push(channel3.port1), "frozen");
    same(evt.ports.length, 1, "length");
    done();
  }), true);
  const channel2 = new MessageChannel();
  channel.port1.postMessage("ports", [channel2.port1]);
});

asyncTest("When the user agent is to clone a port original port, with the clone being owned by owner, it must return a new MessagePort object", (done, step) => {
  const channelA = new MessageChannel();
  const channelB = new MessageChannel();
  const originalPort = channelB.port2;
  channelA.port2.onmessage = step((evt) => {
    if (evt.data === "ports") {
      const clonedPort = evt.ports[0];
      assert(clonedPort !== originalPort, "a new port object");
      clonedPort.onmessage = step((e) => {
        same(e.data, "ping", "data through the new port");
        done();
      });
    }
  });
  channelA.port1.postMessage("ports", [originalPort]);
  channelB.port1.postMessage("ping");
});

test("Throw a DataCloneError if transfer array in postMessage contains source port.", () => {
  const channel = new MessageChannel();
  channel.port1.start();
  throwsDom("DataCloneError", () => channel.port1.postMessage("ports", [channel.port1]), "source port");
});

test("Throw a DataCloneError when a host object (e.g. a DOM node) is used with postMessage.", () => {
  const channel = new MessageChannel();
  channel.port1.start();
  throwsDom("DataCloneError", () => channel.port1.postMessage(globalThis), "globalThis");
});

test("Transferring the entangled target port does not throw", () => {
  const channel = new MessageChannel();
  channel.port1.postMessage("doomed", [channel.port2]);
});

test("Transferring the entangled target port detaches it", () => {
  const channel = new MessageChannel();
  channel.port1.postMessage("doomed", [channel.port2]);
  throwsDom("DataCloneError", () => structuredClone(null, { transfer: [channel.port2] }), "detached");
});

test("Transferring the entangled target port via StructuredSerializeOptions detaches it", () => {
  const channel = new MessageChannel();
  channel.port1.postMessage("doomed", { transfer: [channel.port2] });
  throwsDom("DataCloneError", () => structuredClone(null, { transfer: [channel.port2] }), "detached");
});

asyncTest("An entangled port transferred to the same origin receives messages in order", (done, step) => {
  const channel1 = new MessageChannel();
  const channel2 = new MessageChannel();
  channel1.port1.postMessage(1);
  channel2.port1.postMessage("transfer", [channel1.port1]);
  channel2.port1.postMessage("transfer", [channel1.port2]);
  let transferCounter = 0;
  let sender;
  channel2.port2.onmessage = step((evt) => {
    if (transferCounter === 0) {
      sender = evt.ports[0];
      transferCounter = 1;
    } else {
      sender.postMessage(2);
      let counter = 0;
      evt.ports[0].onmessage = step((e) => {
        same(e.data, counter + 1, `message ${counter + 1}`);
        if (++counter === 3) {
          done();
        }
      });
      sender.postMessage(3);
    }
  });
});

asyncTest("When transferring a non-enabled port mutiple times, incoming messages sent at various transfer steps are received in order upon enablement.", (done, step) => {
  const channel1 = new MessageChannel();
  const channel2 = new MessageChannel();
  const channel3 = new MessageChannel();
  channel2.port2.onmessage = step((evt) => {
    channel3.port1.onmessage = step((evt3) => {
      const expected = ["First", "Second", "Third", "Fourth"];
      let counter = 0;
      evt3.ports[0].onmessage = step((e) => {
        same(e.data, expected[counter], `message ${counter}`);
        if (++counter === 4) {
          done();
        }
      });
      channel1.port2.postMessage("Fourth");
    });
    channel1.port2.postMessage("Second");
    channel1.port2.postMessage("Third");
    channel3.port2.postMessage("2", evt.ports);
  });
  channel1.port2.postMessage("First");
  channel2.port1.postMessage("1", [channel1.port1]);
});

asyncTest("When transferring a port, outgoing messages sent at each transfer step are received in order by the entangled port.", (done, step) => {
  const channel1 = new MessageChannel();
  const channel2 = new MessageChannel();
  const channel3 = new MessageChannel();
  channel2.port2.onmessage = step((evt) => {
    evt.ports[0].postMessage("Second");
    evt.ports[0].postMessage("Third");
    channel3.port1.onmessage = step((evt3) => {
      evt3.ports[0].postMessage("Fourth");
    });
    channel3.port2.postMessage("2", evt.ports);
  });
  channel1.port1.postMessage("First");
  channel2.port1.postMessage("1", [channel1.port1]);
  const expected = ["First", "Second", "Third", "Fourth"];
  let counter = 0;
  channel1.port2.onmessage = step((e) => {
    same(e.data, expected[counter], `message ${counter}`);
    if (++counter === 4) {
      done();
    }
  });
});

// ---------------------------------------------------------------------
// close() (message-channels/close.any.js, historical.any.js,
// transfer-from-closed-port.any.js)
// ---------------------------------------------------------------------

asyncTest("Message sent to closed port should not arrive.", (done, step, failWith) => {
  const c = new MessageChannel();
  c.port1.onmessage = () => failWith("delivered");
  c.port1.close();
  c.port2.postMessage("TEST");
  setTimeout(() => done(), quiet);
});

asyncTest("Message sent from closed port should not arrive.", (done, step, failWith) => {
  const c = new MessageChannel();
  c.port1.onmessage = () => failWith("delivered");
  c.port2.close();
  c.port2.postMessage("TEST");
  setTimeout(() => done(), quiet);
});

asyncTest("Message sent to closed port from transferred port should not arrive.", (done, step, failWith) => {
  const c = new MessageChannel();
  c.port1.onmessage = () => failWith("delivered");
  c.port1.close();
  const c2 = new MessageChannel();
  c2.port1.onmessage = step((e) => {
    e.ports[0].postMessage("TESTMSG");
    setTimeout(() => done(), quiet);
  });
  c2.port2.postMessage("TEST", [c.port2]);
});

asyncTest("Inflight messages should be delivered even when sending port is closed afterwards.", (done, step) => {
  const c = new MessageChannel();
  let isClosed = false;
  c.port1.onmessage = step((e) => {
    assert(isClosed, "closed first");
    same(e.data, "TEST", "data");
    done();
  });
  c.port2.postMessage("TEST");
  c.port2.close();
  isClosed = true;
});

asyncTest("Close in onmessage should not cancel inflight messages.", (done, step) => {
  const c = new MessageChannel();
  c.port1.onmessage = step((e) => {
    if (e.data === "DONE") {
      done();
      return;
    }
    same(e.data, "TEST", "data");
    c.port1.close();
  });
  c.port2.postMessage("TEST");
  c.port2.postMessage("DONE");
});

test("close() detaches a MessagePort (but not the one its entangled with)", () => {
  const c1 = new MessageChannel();
  const c2 = new MessageChannel();
  c1.port1.close();
  throwsDom("DataCloneError", () => c2.port1.postMessage(null, [c1.port1]), "closed port");
  c2.port1.postMessage(null, [c1.port2]);
});

asyncTest("No close event should be fired when an entangled port is closed", (done, step, failWith) => {
  const c = new MessageChannel();
  c.port1.addEventListener("close", () => failWith("close event on port1"));
  c.port2.addEventListener("close", () => failWith("close event on port2"));
  c.port1.start();
  c.port2.start();
  c.port2.close();
  setTimeout(() => done(), quiet);
});

asyncTest("No close event should be fired on a port when it is closed", (done, step, failWith) => {
  const c = new MessageChannel();
  c.port1.onmessage = () => {};
  c.port1.addEventListener("close", () => failWith("close event"));
  c.port1.close();
  setTimeout(() => done(), quiet);
});

test("Transferring a port from a closed port detaches it (transfer sequence)", () => {
  const closedPort = new MessageChannel().port1;
  closedPort.close();
  const { port1 } = new MessageChannel();
  closedPort.postMessage(0, [port1]);
  throwsDom("DataCloneError", () => new MessageChannel().port1.postMessage(0, [port1]), "detached");
});

test("Transferring a port from a closed port detaches it (StructuredSerializeOptions)", () => {
  const closedPort = new MessageChannel().port1;
  closedPort.close();
  const { port1 } = new MessageChannel();
  closedPort.postMessage(0, { transfer: [port1] });
  throwsDom("DataCloneError", () => structuredClone(0, { transfer: [port1] }), "detached");
});

test("Transferring both ports of a channel from a closed port detaches both", () => {
  const closedPort = new MessageChannel().port1;
  closedPort.close();
  const { port1, port2 } = new MessageChannel();
  closedPort.postMessage(0, [port1, port2]);
  throwsDom("DataCloneError", () => structuredClone(0, { transfer: [port1] }), "port1");
  throwsDom("DataCloneError", () => structuredClone(0, { transfer: [port2] }), "port2");
});

test("Transferring a port and an ArrayBuffer from a closed port detaches both", () => {
  const closedPort = new MessageChannel().port1;
  closedPort.close();
  const buffer = new ArrayBuffer(8);
  const { port1 } = new MessageChannel();
  closedPort.postMessage(0, [buffer, port1]);
  same(buffer.byteLength, 0, "buffer detached");
  throwsDom("DataCloneError", () => structuredClone(0, { transfer: [port1] }), "port detached");
});

test("Transferring the formerly entangled port from a closed port detaches it", () => {
  const { port1, port2 } = new MessageChannel();
  port1.close();
  port1.postMessage(0, [port2]);
  throwsDom("DataCloneError", () => structuredClone(0, { transfer: [port2] }), "detached");
});

// ---------------------------------------------------------------------
// Forge's own checks
// ---------------------------------------------------------------------

test("postMessage argument handling", () => {
  const { port1 } = new MessageChannel();
  throwsType(TypeError, () => port1.postMessage(), "no message");
  throwsType(TypeError, () => port1.postMessage(1, 5), "a number as transfer");
  throwsType(TypeError, () => port1.postMessage(1, { transfer: {} }), "transfer not iterable");
  throwsType(TypeError, () => port1.postMessage(1, [1]), "a transfer item that is not an object");
  port1.postMessage(1, null);
  port1.postMessage(1, undefined);
  port1.postMessage(1, {});
  throwsDom("DataCloneError", () => port1.postMessage(() => {}), "a function");
  throwsDom("DataCloneError", () => port1.postMessage(new Event("x")), "a platform object");
});

asyncTest("messages are copies, delivered in order, one task each", (done, step) => {
  const { port1, port2 } = new MessageChannel();
  const sent = { list: [1, 2] };
  const received = [];
  const microtasks = [];
  port2.onmessage = step((e) => {
    received.push(e.data.n);
    queueMicrotask(() => microtasks.push(e.data.n));
    if (e.data.n === 0) {
      assert(e.data.sent !== sent && e.data.sent.list[1] === 2, "a structured clone");
      same(microtasks.length, 0, "first message");
    } else {
      // The previous message's microtask ran before this task.
      same(microtasks[microtasks.length - 1], e.data.n - 1, "a microtask checkpoint between messages");
    }
    if (received.length === 1000) {
      for (let i = 0; i < 1000; i++) {
        if (received[i] !== i) {
          throw new Error(`message ${i} arrived as ${received[i]}`);
        }
      }
      done();
    }
  });
  for (let i = 0; i < 1000; i++) {
    port1.postMessage({ n: i, sent });
  }
  sent.list[1] = 99;  // after posting: not seen by the receiver
}, 2000);

asyncTest("messageerror is not fired for ordinary messages; a listener can be added both ways", (done, step, failWith) => {
  const { port1, port2 } = new MessageChannel();
  port2.onmessageerror = () => failWith("messageerror");
  port2.addEventListener("message", step(() => done()));
  port2.start();
  port1.postMessage(new Uint8Array([1, 2]));
});

asyncTest("a transferred port's listeners stay with the old object", (done, step, failWith) => {
  const { port1, port2 } = new MessageChannel();
  port2.onmessage = () => failWith("the old object received a message");
  const moved = structuredClone(port2, { transfer: [port2] });
  same(port2.onmessage !== null, true, "the old handler is still set");
  moved.onmessage = step((e) => {
    same(e.data, "to the new object", "data");
    done();
  });
  port1.postMessage("to the new object");
  port2.postMessage("from the old object goes nowhere");
});

asyncTest("ports sent in a message arrive in transfer order, buffers are not ports", (done, step) => {
  const { port1, port2 } = new MessageChannel();
  const a = new MessageChannel();
  const b = new MessageChannel();
  const buffer = new ArrayBuffer(3);
  port2.onmessage = step((e) => {
    same(e.ports.length, 2, "two ports");
    same(e.data.b, e.ports[1], "the message's reference is the same port object");
    same(e.data.buffer.byteLength, 3, "buffer");
    done();
  });
  port1.postMessage({ b: b.port1, buffer }, [a.port1, buffer, b.port1]);
});

asyncTest("a message dropped with its ports disentangles them", (done, step) => {
  const { port1 } = new MessageChannel();
  port1.close();
  const c = new MessageChannel();
  port1.postMessage("dropped", [c.port1]);  // no target: dropped, c.port1 travels nowhere
  c.port2.postMessage("lost");               // c.port2 is no longer entangled
  throwsDom("DataCloneError", () => structuredClone(0, { transfer: [c.port1] }), "c.port1 detached");
  setTimeout(() => done(), quiet);
});

test("changing the built-ins does not change messaging", () => {
  const saved = Object.getPrototypeOf(MessageEvent);
  let called = false;
  Object.setPrototypeOf(MessageEvent, function Evil() { called = true; });
  Object.defineProperty(Array.prototype, "0", {
    set() { called = true; }, get() { return undefined; }, configurable: true,
  });
  try {
    const { port1, port2 } = new MessageChannel();
    const extra = new MessageChannel();
    port2.start();
    port1.postMessage(1, new Set([extra.port1]));
  } finally {
    delete Array.prototype[0];
    Object.setPrototypeOf(MessageEvent, saved);
  }
  same(called, false, "no user code ran");
});

Promise.all(pending).then(() => {
  console.log(`messaging done: ${checks} checks, ${failures} failed`);
  if (failures > 0) {
    Forge.exitCode = 1;
  }
});
