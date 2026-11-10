// Press Ctrl+C once. Expected: "got SIGINT", then "cleaned up", exit code 0.
console.log("waiting for Ctrl+C...");
const timer = setInterval(() => {}, 1000);
Forge.addSignalListener("SIGINT", (name) => {
  console.log("got " + name);
  clearInterval(timer);
  setTimeout(() => console.log("cleaned up"), 10);
});
