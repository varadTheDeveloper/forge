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
