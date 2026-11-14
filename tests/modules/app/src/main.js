// Phase 9.1: the entry module of the module runtime test. Run with no
// permissions; see ../../README.md for the folder layout.
import { add, bump, counter, value } from "./lib/math.js";
import * as ns from "./lib/math.js";
import { value as again } from "./lib/../lib/math.js";
import "./lib/side-effect.js";
import data from "./data/config.json" with { type: "json" };
import { shout } from "../shared/util.js";
import { bom } from "./lib/bom.js";

const lines = [];
const log = (text) => lines.push(text);

log("add: " + add(2, 3));
bump();
log("live binding: " + counter);
log("same module: " + (ns.add === add && again === value));
log("side effect runs: " + globalThis.sideEffectCount);
log("json: " + data.name + " " + data.items.length);
log("shared: " + shout("root"));
log("bom: " + bom);
log("meta url: " + import.meta.url.startsWith("file:///") + " " +
    import.meta.url.endsWith("/app/src/main.js"));
log("meta main: " + import.meta.main);
log("resolve: " + import.meta.resolve("./lib/math.js").endsWith("/app/src/lib/math.js"));
log("strict: " + (function () { return this === undefined; })());

log("tla: " + (await Promise.resolve("tla-ok")));

const dynamic = await import("./lib/dynamic.js");
log("dynamic: " + dynamic.default + " main=" + dynamic.isMain);

let order = "sync";
const pending = import("./lib/dynamic.js").then(() => { order += ",import"; });
order += ",after";
await pending;
log("import is async: " + order);

const base = import.meta.url.slice(0, import.meta.url.length - "app/src/main.js".length);

async function refused(label, specifier, options) {
  try {
    await import(specifier, options);
    log("SHOULD NOT LOAD " + label);
  } catch (e) {
    log("refused " + label + ": " + (e.code ?? e.name));
  }
}

await refused("node", "node:fs");
await refused("bare", "lodash");
await refused("https", "https://example.com/x.js");
await refused("query", "./lib/math.js?v=1");
await refused("windows path", "C:/Windows/win.ini");
await refused("json without type", "./data/config.json");
await refused("css type", "./lib/math.js", { with: { type: "css" } });
await refused("missing", "./lib/missing.js");
await refused("escape", "../../outside.js");
await refused("file url", base + "outside.js");
await refused("junction", "../out-link/evil.js");
await refused("protected", "../.git/hook.js");
await refused("syntax", "./lib/syntax-error.js");

try {
  await import("./lib/throws.js");
  log("SHOULD NOT LOAD throws");
} catch (e) {
  log("throws: " + e.message);
}

try {
  import.meta.resolve(42);
  log("SHOULD NOT RESOLVE a number");
} catch (e) {
  log("resolve number: " + e.code);
}

console.log(lines.join("\n"));
console.log("modules done");
