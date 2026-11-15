// Phase 9.4: the entry module of the per-package permission test. Run by
// run-runtime-tests.ps1 with --allow-read for ./data and ./private, two
// --allow-env names and an audit log; Forge.args[0] is this program's folder.
import * as reader from "reader";
import * as greedy from "greedy";
import * as capped from "capped";
import * as wanting from "wanting";

const appDir = Forge.args[0];
const dataFile = appDir + "\\data\\note.txt";
const secretFile = appDir + "\\private\\secret.txt";
const otherFile = appDir + "\\other\\x.txt";
const outFile = appDir + "\\data\\out.txt";

const lines = [];
const log = (text) => lines.push(text);

// Which of the three layers (D26) refused, from Kavach's message.
function layerOf(message) {
  if (message.includes("does not declare")) return "not-declared";
  if (message.includes("does not grant")) return "not-granted";
  if (message.includes("command line does not allow")) return "command-line";
  if (message.includes("cannot read every environment variable")) return "all-variables";
  return "-";
}

function report(label, e) {
  log(label + ": " + e.code + " " + (e.package ?? "-") + " " + layerOf(String(e.message)));
}

function attempt(label, action) {
  try {
    log(label + ": ok " + action());
  } catch (e) {
    report(label, e);
  }
}

async function attemptAsync(label, action) {
  try {
    log(label + ": ok " + (await action()));
  } catch (e) {
    report(label, e);
  }
}

// The program itself may read the private file (the command line allows it).
attempt("app reads secret", () => fs.readFileSync(secretFile, "utf8"));

log("reader globals: " + reader.globals());
attempt("reader data", () => reader.read(dataFile));
attempt("reader secret", () => reader.read(secretFile));
attempt("reader write", () => reader.write(outFile));
attempt("reader env", () => reader.env("FORGE_PERM_TEST"));
attempt("reader other env", () => reader.env("FORGE_PERM_OTHER"));
attempt("reader env all", () => reader.envAll());
await attemptAsync("reader async data", () => reader.readAsync(dataFile));
await attemptAsync("reader async secret", () => reader.readAsync(secretFile));
attempt("reader leaks fs", () => typeof reader.leakFs());
attempt("reader wasm data", () => reader.wasmRead(dataFile));
attempt("reader wasm secret", () => reader.wasmRead(secretFile));

attempt("greedy data", () => greedy.read(dataFile));
attempt("greedy secret", () => greedy.read(secretFile));
log("greedy steals from reader: " + greedy.stealFromReader());

attempt("capped other", () => capped.read(otherFile));

log("wanting globals: " + wanting.globals());

console.log(lines.join("\n"));
console.log("permissions done");
