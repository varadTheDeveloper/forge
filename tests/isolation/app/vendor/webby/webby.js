// Phase 10.1: the web APIs inside a package compartment, and web API
// objects crossing between compartments (see ../../README.md).
export const checks = [];
const log = (text) => checks.push(text);

log("globals: " + [typeof DOMException, typeof TextEncoder, typeof TextDecoder, typeof atob,
                   typeof btoa].join(" "));
log("frozen: " + [Object.isFrozen(DOMException), Object.isFrozen(DOMException.prototype),
                  Object.isFrozen(TextDecoder.prototype), Object.isFrozen(TextEncoder)].join(" "));
log("works: " + new TextDecoder().decode(new TextEncoder().encode("hé")) + " " +
    atob(btoa("ok")) + " " + new DOMException("m", "AbortError").code);

try {
  TextDecoder.prototype.decode = () => "hijacked";
  log("SHOULD NOT REPLACE decode");
} catch (e) {
  log("replace decode: " + e.name);
}

export const PackageDOMException = DOMException;

export function makeDomException() {
  return new DOMException("from package", "NotFoundError");
}

export function makeDecoder() {
  return new TextDecoder("utf-16le", { fatal: true });
}

export function makeEncoder() {
  return new TextEncoder();
}

export function throwDom() {
  throw new DOMException("thrown in package", "AbortError");
}

// The program's objects, used in here.
export function decodeWith(decoder, bytes) {
  return decoder.decode(bytes);
}

export function describeDom(e) {
  return (e instanceof DOMException) + " " + e.name + " " + e.message + " " + e.code;
}

// Phase 10.2: events inside a package. The package's global is an
// EventTarget too, but uncaught errors and rejections are dispatched only at
// the program's global: a package never sees (or cancels) them.
log("events: " + [globalThis instanceof EventTarget, self === globalThis, navigator.userAgent,
                  Object.isFrozen(EventTarget.prototype),
                  Object.isFrozen(Object.getPrototypeOf(navigator))].join(" "));

let packageSawRejection = 0;
addEventListener("unhandledrejection", () => { packageSawRejection++; });

export function rejectInPackage() {
  Promise.reject(new Error("from webby"));
}

export function packageSawRejections() {
  return packageSawRejection;
}

export function makeTarget() {
  return new EventTarget();
}

export function fire(target, type) {
  return target.dispatchEvent(new Event(type));
}

// Phase 10.3: abort signals inside a package, and signals of another
// compartment (the program's) used with this package's own objects.
log("abort: " + [typeof AbortController, typeof AbortSignal, Object.isFrozen(AbortSignal.prototype),
                 Object.isFrozen(AbortSignal), AbortSignal.abort("p").reason].join(" "));

export function makeController() {
  return new AbortController();
}

// A listener on a package EventTarget, removed by `signal`.
export function listenUntil(signal) {
  const target = new EventTarget();
  let count = 0;
  target.addEventListener("ping", () => { count++; }, { signal });
  return () => {
    target.dispatchEvent(new Event("ping"));
    return count;
  };
}

// A package signal that follows `signal`.
export function follow(signal) {
  const dependent = AbortSignal.any([signal]);
  const seen = [];
  dependent.onabort = () => { seen.push(String(dependent.reason)); };
  return () => dependent.aborted + " " + seen.join();
}

export function alreadyAborted(signal) {
  const target = new EventTarget();
  let count = 0;
  target.addEventListener("ping", () => { count++; }, { signal });
  target.dispatchEvent(new Event("ping"));
  return count + " " + AbortSignal.any([signal]).reason;
}

export function badSignal() {
  try {
    new EventTarget().addEventListener("ping", () => {}, { signal: new EventTarget() });
    return "SHOULD NOT ACCEPT";
  } catch (e) {
    return e.name;
  }
}

// Phase 10.4: structuredClone and messaging inside a package, and ports of
// another compartment (the program's) used from here.
log("messaging: " + [typeof structuredClone, typeof MessageChannel, Object.isFrozen(MessagePort.prototype),
                     structuredClone({ deep: [1, new Map([[2, 3]])] }).deep[1].get(2)].join(" "));

export function postTo(port, value) {
  port.postMessage(value);
}

export function listenOn(port) {
  const seen = [];
  port.onmessage = (e) => seen.push(JSON.stringify(e.data));
  return () => seen.join(" ");
}

export function makeChannel() {
  return new MessageChannel();
}

export function cloneHere(value) {
  try {
    return "cloned " + JSON.stringify(structuredClone(value));
  } catch (e) {
    return e.name;
  }
}
