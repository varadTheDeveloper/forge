// Phase 10.1: an uncaught DOMException is reported like any other error:
// its name and message, and a location in this file, never in Forge's own
// script (runtime/WebApis.cpp, MakeError).

function thrower() {
  atob("not base64!");
}

console.log("before");
thrower();
console.log("SHOULD NOT RUN");
