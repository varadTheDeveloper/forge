// Expect: exit code 1. A handler attached only in a later timer is too
// late: the rejection is reported once the microtask queue drains (Node
// behaves the same way).
const p = Promise.reject(new Error("handled too late"));
setTimeout(() => p.catch(() => {}), 0);
