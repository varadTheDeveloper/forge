// Phase 10.2: an uncaught error or an unhandled rejection is first
// dispatched as an event at the global object. A listener that cancels it
// keeps the program running (exit code 0).
const log = (text) => console.log(text);
let errors = 0;

addEventListener("error", (e) => {
  errors++;
  log(`error event: ${e.error && e.error.message} | ${e instanceof ErrorEvent} ${e.isTrusted} ` +
      `${e.cancelable} | ${e.message.includes(e.error && e.error.message)} ` +
      `${e.filename.endsWith("web-error-handled.js")} ${e.lineno > 0}`);
  e.preventDefault();
});

onunhandledrejection = (e) => {
  log(`unhandledrejection: ${e.reason.message} | ${e instanceof PromiseRejectionEvent} ` +
      `${e.isTrusted} ${e.promise instanceof Promise}`);
  return false;  // an ordinary event handler cancels by returning false
};

onrejectionhandled = (e) => log(`rejectionhandled: ${e.reason.message} ${e.cancelable}`);

setTimeout(() => { throw new Error("from a timer"); }, 0);
queueMicrotask(() => { throw new Error("from a microtask"); });
const rejected = Promise.reject(new Error("rejected"));
setTimeout(() => rejected.catch(() => log("handled late")), 10);
setTimeout(() => log(`still running: ${errors} errors`), 40);

const target = new EventTarget();
target.addEventListener("x", () => { throw new Error("from a listener"); });
target.dispatchEvent(new Event("x"));
log("after dispatchEvent");
reportError(new Error("reported"));
log("after reportError");

throw new Error("top-level, cancelled");
