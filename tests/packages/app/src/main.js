// Phase 9.2: the entry module of the package runtime test. Run with no
// permissions; see ../../README.md for the packages and what each tests.
import greeting, { utilMarker, selfExtra } from "greet";
import { extra } from "greet/extra";
import { feature } from "greet/features/alpha";
import { marker } from "@forge-test/util";
import outside from "outside";

const lines = [];
const log = (text) => lines.push(text);

log("greet: " + greeting);
log("condition: " + extra + " self: " + selfExtra);
log("pattern: " + feature);
log("one util instance: " + (marker === utilMarker));
log("outside: " + outside);
log("resolve: " + import.meta.resolve("greet").endsWith("/vendor/greet/index.js"));

async function refused(label, specifier) {
  try {
    await import(specifier);
    log("SHOULD NOT LOAD " + label);
  } catch (e) {
    log("refused " + label + ": " + (e.code ?? e.name));
  }
}

await refused("not exported", "greet/internal.js");
await refused("private", "greet/private");
await refused("into a package by path", "../vendor/greet/internal.js");
await refused("package escaping by path", "greet/escape");
await refused("undeclared", "sneaky");
await refused("dependency outside the program", "escaper");
await refused("duplicate key", "broken");
await refused("misnamed", "misnamed");
await refused("phantom", "phantom-user");
await refused("no exports", "no-exports");
await refused("invalid name", "Greet");
await refused("unsafe subpath", "greet/../vendor");
await refused("self without exports", "pkg-test-app");

try {
  import.meta.resolve("greet/nope");
  log("SHOULD NOT RESOLVE greet/nope");
} catch (e) {
  log("resolve not exported: " + e.code);
}

console.log(lines.join("\n"));
console.log("packages done");
