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
