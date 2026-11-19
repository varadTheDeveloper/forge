// Phase 10.3: timers fire in order of due time, and timers due at the same
// time in the order they were scheduled, as in browsers and Node.
// Expect: exit code 0 and "timer order: 123 abc".
// (forge-core's TimerScheduler used to fire due timers in vector order,
// which its swap-removal scrambled: these printed "132" and "acb" or worse.)
let same = "";
let staggered = "";
const far = setTimeout(() => {}, 100000);
setTimeout(() => { same += "1"; }, 5);
setTimeout(() => { same += "2"; }, 5);
setTimeout(() => { same += "3"; }, 5);
setTimeout(() => { staggered += "c"; }, 30);
setTimeout(() => { staggered += "a"; }, 10);
setTimeout(() => { staggered += "b"; }, 20);
// Block until every timer above is due, so they all fire in one pass of
// the event loop and only the scheduler's order decides.
const until = Date.now() + 60;
while (Date.now() < until) {
  // busy wait
}
setTimeout(() => {
  clearTimeout(far);
  print("timer order: " + same + " " + staggered);
}, 0);
