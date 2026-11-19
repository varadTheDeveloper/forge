// Phase 10.3.1: a microtask checkpoint after every task (HTML event loop).
// Promise reactions and queueMicrotask callbacks queued by a task run
// before the next task, even when both tasks are ready in the same pass
// of Forge's event loop; unhandled rejections are reported at that
// checkpoint too. Forge.args[0] is a folder Forge may read.
// Expect: exit code 0 and
//   "timers: t1,m1,ur,t2,m2"  and  "io before timer: io,t3".
// Before Phase 10.3.1 these printed "t1,t2,m1,m2,ur" and "t3,io".

function busyWait(ms) {
  const until = Date.now() + ms;
  while (Date.now() < until) {
    // keep the event loop from running, so the tasks pile up
  }
}

const seen = [];
addEventListener("unhandledrejection", (e) => {
  seen.push("ur");
  e.preventDefault();
});

setTimeout(() => {
  seen.push("t1");
  Promise.resolve().then(() => seen.push("m1"));
  Promise.reject(new Error("reported at t1's checkpoint"));
}, 5);
setTimeout(() => {
  seen.push("t2");
  queueMicrotask(() => seen.push("m2"));
}, 5);
busyWait(40);

setTimeout(() => {
  console.log("timers: " + seen.join(","));

  // An async read that completes while a timer also becomes due: the
  // read's promise reaction runs before the timer.
  const order = [];
  fs.readdir(Forge.args[0]).then(() => order.push("io"));
  setTimeout(() => {
    order.push("t3");
    setTimeout(() => console.log("io before timer: " + order.join(",")), 0);
  }, 5);
  busyWait(200);
}, 0);
