// Expect: exit code 0 and exactly "order: sync,promise,micro1,micro2,timer".
const seen = [];
setTimeout(() => {
  seen.push("timer");
  print("order: " + seen.join(","));
}, 0);
Promise.resolve().then(() => seen.push("promise"));
queueMicrotask(() => seen.push("micro1"));
queueMicrotask(() => seen.push("micro2"));
seen.push("sync");
