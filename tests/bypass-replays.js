// Phase 8.4: replays of permission bypasses found in other runtimes, and of
// Windows path tricks, against Kavach. BYPASS-REPLAYS.md explains each one.
//
//   Forge.args[0]: G, a folder granted for reading and writing (and the
//                  current folder). The runner puts in it: .git\config,
//                  forge.json, ok.txt, victim\keep.txt, out-link (a junction
//                  to O), git-link (a junction to G\.git), hard.txt (a hard
//                  link to O\secret.txt) and the audit log audit.jsonl.
//   Forge.args[1]: O, a folder outside every grant, holding secret.txt.
//   Forge.args[2]: the audit log path.
//
// Forge runs with --allow-read=G --allow-write=G
// --allow-env=FORGE_REPLAY_GRANTED --audit-log=G\audit.jsonl.
//
// Prints one line per replay ("blocked" or "BYPASSED ..."), then
// "replays done: <n> blocked, <m> bypassed". The runner also checks that
// nothing outside G and none of the protected files changed.

"use strict";

const [G, O, AUDIT] = Forge.args;
const SECRET = O + "\\secret.txt";
const REFUSED = ["PermissionDenied", "InvalidArgument"];

let blocked = 0;
let bypassed = 0;
const pending = [];

function show(value) {
  if (typeof value === "string") {
    return JSON.stringify(value.length > 60 ? value.slice(0, 60) + "..." : value);
  }
  if (value instanceof Uint8Array) {
    return "Uint8Array(" + value.length + ")";
  }
  return String(value);
}

// Runs every check of one replay; a check fails when the operation
// succeeds or fails with a code outside `codes`.
function replay(id, title, checks, codes = REFUSED) {
  const problems = [];

  for (const [what, fn] of checks) {
    try {
      const result = fn();
      problems.push(`${what} succeeded (${show(result)})`);
    } catch (e) {
      if (!codes.includes(e && e.code)) {
        problems.push(`${what} failed with ${e && e.code}: ${e && e.message}`);
      }
    }
  }

  report(id, title, problems, checks.length);
}

function report(id, title, problems, count) {
  if (problems.length === 0) {
    blocked++;
    console.log(`replay ${id} ${title}: blocked (${count} checks)`);
  } else {
    bypassed++;
    console.log(`replay ${id} ${title}: BYPASSED ${problems.join("; ")}`);
  }
}

// The same for Promise-returning functions: they must reject.
function replayAsync(id, title, checks, codes = REFUSED) {
  pending.push((async () => {
    const problems = [];
    for (const [what, fn] of checks) {
      try {
        const result = await fn();
        problems.push(`${what} resolved (${show(result)})`);
      } catch (e) {
        if (!codes.includes(e && e.code)) {
          problems.push(`${what} rejected with ${e && e.code}: ${e && e.message}`);
        }
      }
    }
    report(id, title, problems, checks.length);
  })());
}

const ascii = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0) & 0xff);

// R01: "..", in every spelling, cannot leave the grant.
replay("R01", "path traversal", [
  ["read G\\..\\O", () => fs.readFileSync(G + "\\..\\outside\\secret.txt", "utf8")],
  ["read G/sub/../../O", () => fs.readFileSync(G + "/victim/../../outside/secret.txt", "utf8")],
  ["read relative ..", () => fs.readFileSync("..\\outside\\secret.txt", "utf8")],
  ["read mixed separators", () => fs.readFileSync(G + "\\\\..//outside\\\\secret.txt", "utf8")],
  ["write G\\..\\O", () => fs.writeFileSync(G + "\\..\\outside\\new.txt", "x")],
  ["readdir ..", () => fs.readdirSync("..")],
]);

// R02: a path given as bytes (Node CVE-2023-32004 with Buffer,
// CVE-2023-39332 with Uint8Array). Forge accepts strings only.
replay("R02", "path as bytes", [
  ["read Uint8Array path", () => fs.readFileSync(ascii(SECRET))],
  ["read Uint8Array of a granted file", () => fs.readFileSync(ascii(G + "\\ok.txt"))],
  ["read ArrayBuffer path", () => fs.readFileSync(ascii(SECRET).buffer)],
  ["read DataView path", () => fs.readFileSync(new DataView(ascii(SECRET).buffer))],
  ["write Uint8Array path", () => fs.writeFileSync(ascii(O + "\\new.txt"), "x")],
], ["InvalidArgument"]);

// R03: a path object whose conversion to a string changes over time
// (check one string, use another). Forge never converts: it refuses.
{
  let calls = 0;
  const shifty = { toString() { calls++; return calls === 1 ? G + "\\ok.txt" : SECRET; } };
  const primitive = { [Symbol.toPrimitive]() { calls++; return SECRET; } };
  replay("R03", "path object with toString", [
    ["read {toString}", () => fs.readFileSync(shifty, "utf8")],
    ["read {Symbol.toPrimitive}", () => fs.readFileSync(primitive, "utf8")],
    ["read new String()", () => fs.readFileSync(new String(SECRET), "utf8")],
    ["read [path]", () => fs.readFileSync([SECRET], "utf8")],
    ["toString was never called", () => { if (calls === 0) throw { code: "InvalidArgument" }; return calls; }],
  ], ["InvalidArgument"]);
}

// R04: a network path is refused before any connection is made, so
// Windows never sends the user's credentials (NTLM) to that host.
// 192.0.2.1 is a documentation address that is never routed; a connection
// attempt would take many seconds.
{
  const start = Date.now();
  replay("R04", "network path, no connection", [
    ["read \\\\192.0.2.1\\share", () => fs.readFileSync("\\\\192.0.2.1\\share\\x.txt")],
    ["read //192.0.2.1/share", () => fs.readFileSync("//192.0.2.1/share/x.txt")],
    ["stat \\\\name.invalid\\share", () => fs.statSync("\\\\forge-replay.invalid\\share\\x")],
    ["exists \\\\192.0.2.1\\share", () => {
      if (fs.existsSync("\\\\192.0.2.1\\share\\x")) return "true";
      throw { code: "PermissionDenied" };
    }],
    ["write \\\\192.0.2.1\\share", () => fs.writeFileSync("\\\\192.0.2.1\\share\\x.txt", "x")],
    ["refused in under 3 seconds", () => {
      const ms = Date.now() - start;
      if (ms < 3000) throw { code: "PermissionDenied" };
      return ms + " ms";
    }],
  ]);
}

// R05: device and namespace paths (the Windows side of Deno
// CVE-2024-34346, where /proc/self/mem and similar device files gave more
// than the grant). Refused even when they name a granted file.
replay("R05", "device and namespace paths", [
  ["read \\\\?\\ + granted file", () => fs.readFileSync("\\\\?\\" + G + "\\ok.txt")],
  ["read \\\\?\\ + secret", () => fs.readFileSync("\\\\?\\" + SECRET)],
  ["read \\\\.\\ + granted file", () => fs.readFileSync("\\\\.\\" + G + "\\ok.txt")],
  ["read \\??\\ path", () => fs.readFileSync("\\??\\" + SECRET)],
  ["read GLOBALROOT", () => fs.readFileSync("\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\Windows\\win.ini")],
  ["read \\\\.\\PhysicalDrive0", () => fs.readFileSync("\\\\.\\PhysicalDrive0")],
  ["read \\\\.\\pipe", () => fs.readFileSync("\\\\.\\pipe\\forge-replay")],
  ["read CON", () => fs.readFileSync("CON")],
  ["read G\\NUL.txt", () => fs.readFileSync(G + "\\NUL.txt")],
  ["write G\\COM1", () => fs.writeFileSync(G + "\\COM1", "x")],
  ["read CONIN$", () => fs.readFileSync("CONIN$")],
  ["read drive-relative C:file", () => fs.readFileSync(G.slice(0, 2) + "ok.txt")],
]);

// R06: 8.3 short names (GIT~1 for .git, FORGE~1.JSO for forge.json, on
// volumes that create short names) cannot reach a protected path. Where a
// short name exists it resolves to the protected name; where it does not,
// a name that looks like one is refused.
replay("R06", "short-name aliases", [
  ["read GIT~1\\config", () => fs.readFileSync("GIT~1\\config", "utf8")],
  ["write GIT~1\\config", () => fs.writeFileSync("GIT~1\\config", "x")],
  ["mkdir GIT~1", () => fs.mkdirSync("GIT~1")],
  ["rm GIT~1", () => fs.rmSync("GIT~1", { recursive: true })],
  ["rename GIT~1", () => fs.renameSync("GIT~1", "not-git")],
  ["write FORGE~1.JSO", () => fs.writeFileSync("FORGE~1.JSO", "x")],
  ["rm FORGE~1.JSO", () => fs.rmSync("FORGE~1.JSO")],
  ["write AUDIT~1.JSO", () => fs.writeFileSync("AUDIT~1.JSO", "")],
  ["rm AUDIT~1.JSO", () => fs.rmSync("AUDIT~1.JSO")],
]);

// R07: other spellings of protected names. Windows drops trailing dots and
// spaces and ignores case; "::$DATA" names the file's main stream.
replay("R07", "protected-name spellings", [
  ["write forge.json.", () => fs.writeFileSync("forge.json.", "x")],
  ["write \"forge.json \"", () => fs.writeFileSync("forge.json ", "x")],
  ["write FORGE.JSON", () => fs.writeFileSync("FORGE.JSON", "x")],
  ["write forge.json::$DATA", () => fs.writeFileSync("forge.json::$DATA", "x")],
  ["write forge.json:stream", () => fs.writeFileSync("forge.json:stream", "x")],
  ["write victim\\..\\forge.json", () => fs.writeFileSync("victim\\..\\forge.json", "x")],
  ["write .GIT\\config", () => fs.writeFileSync(".GIT\\config", "x")],
  ["write .git.\\config", () => fs.writeFileSync(".git.\\config", "x")],
  ["write .git\\hooks\\pre-commit", () => fs.writeFileSync(".git\\hooks\\pre-commit", "x")],
  ["rename onto forge.json", () => fs.renameSync("ok.txt", "Forge.Json")],
  ["copy onto forge.lock", () => fs.copyFileSync("ok.txt", "forge.lock")],
  ["rm forge.json", () => fs.rmSync("forge.json")],
  ["read .git\\config", () => fs.readFileSync(".git\\config", "utf8")],
], ["PermissionDenied", "InvalidArgument"]);

// R08: a junction inside the grant that points outside it.
replay("R08", "junction out of the grant", [
  ["read out-link\\secret.txt", () => fs.readFileSync("out-link\\secret.txt", "utf8")],
  ["stat out-link\\secret.txt", () => fs.statSync("out-link\\secret.txt")],
  ["readdir out-link", () => fs.readdirSync("out-link")],
  ["realpath out-link\\secret.txt", () => fs.realpathSync("out-link\\secret.txt")],
  ["exists out-link\\secret.txt", () => {
    if (fs.existsSync("out-link\\secret.txt")) return "true";
    throw { code: "PermissionDenied" };
  }],
  ["write out-link\\new.txt", () => fs.writeFileSync("out-link\\new.txt", "x")],
  ["write out-link\\secret.txt", () => fs.writeFileSync("out-link\\secret.txt", "x")],
  ["append out-link\\secret.txt", () => fs.appendFileSync("out-link\\secret.txt", "x")],
  ["mkdir out-link\\d", () => fs.mkdirSync("out-link\\d")],
  ["rename out of out-link", () => fs.renameSync("out-link\\secret.txt", "stolen.txt")],
  ["copy out of out-link", () => fs.copyFileSync("out-link\\secret.txt", "stolen.txt")],
  ["rm out-link\\secret.txt", () => fs.rmSync("out-link\\secret.txt")],
]);

// R09: a junction inside the grant that points at the protected .git.
replay("R09", "junction to .git", [
  ["read git-link\\config", () => fs.readFileSync("git-link\\config", "utf8")],
  ["write git-link\\config", () => fs.writeFileSync("git-link\\config", "x")],
  ["write git-link\\hooks\\post-checkout", () => fs.writeFileSync("git-link\\hooks\\post-checkout", "x")],
  ["readdir git-link", () => fs.readdirSync("git-link")],
], ["PermissionDenied"]);

// R10: prototype pollution must not change what an operation does.
{
  const problems = [];
  Object.prototype.recursive = true;
  Object.prototype.withFileTypes = true;
  try {
    try {
      fs.rmSync("victim", {});
      problems.push("rmSync(victim, {}) deleted a folder with contents");
    } catch (e) {
      if (e.code !== "InvalidOperation") problems.push("rmSync failed with " + e.code);
    }
    try {
      fs.rmSync("victim");
      problems.push("rmSync(victim) deleted a folder with contents");
    } catch (e) {
      if (e.code !== "InvalidOperation") problems.push("rmSync failed with " + e.code);
    }
    if (typeof fs.readdirSync("victim")[0] !== "string") {
      problems.push("readdirSync used an inherited withFileTypes");
    }
  } finally {
    delete Object.prototype.recursive;
    delete Object.prototype.withFileTypes;
  }
  if (!fs.existsSync("victim\\keep.txt")) problems.push("victim\\keep.txt is gone");
  report("R10", "prototype pollution of options", problems, 4);
}

// R11: no ambient authority. Nothing Node, Deno or Bun offer is present,
// and modules cannot be loaded to get it.
{
  const problems = [];
  for (const name of ["process", "require", "module", "exports", "Deno", "Bun", "fetch",
                      "XMLHttpRequest", "WebSocket", "importScripts", "os", "child_process",
                      "Buffer", "global"]) {
    if (typeof globalThis[name] !== "undefined") problems.push(name + " exists");
  }
  report("R11", "no ambient authority", problems, 14);
  // Before Phase 8.4 this Promise never settled (no module load hook).
  pending.push(import("node:fs").then(
    () => report("R11b", "no module loading", ["import(\"node:fs\") resolved"], 1),
    (e) => report("R11b", "no module loading",
                  e && e.code === "NotSupported" ? [] : ["rejected with " + (e && e.code)], 1)));
}

// R12: environment variables are checked per name; tricks with NUL,
// "=" and the hidden per-drive variables do not widen a grant.
replay("R12", "environment names", [
  ["get PATH", () => Forge.env.get("PATH")],
  ["has PATH", () => Forge.env.has("PATH")],
  ["get with NUL", () => Forge.env.get("FORGE_REPLAY_GRANTED\0PATH")],
  ["get with =", () => Forge.env.get("FORGE_REPLAY_GRANTED=PATH")],
  ["get hidden =C:", () => Forge.env.get("=C:")],
  ["get empty name", () => Forge.env.get("")],
  ["toObject with a list grant", () => Forge.env.toObject()],
]);
{
  const problems = [];
  if (Forge.env.get("FORGE_REPLAY_GRANTED") !== "granted-value") problems.push("granted name");
  if (Forge.env.get("forge_replay_granted") !== "granted-value") problems.push("names ignore case");
  report("R12b", "granted environment name still works", problems, 2);
}

// R13: the audit log is protected even inside a granted folder, so a
// script cannot erase the record of what it did.
replay("R13", "audit log protected", [
  ["write the log", () => fs.writeFileSync(AUDIT, "")],
  ["append to the log", () => fs.appendFileSync(AUDIT, "{\"decision\":\"allow\"}\n")],
  ["rm the log", () => fs.rmSync(AUDIT)],
  ["rename the log", () => fs.renameSync(AUDIT, "moved.jsonl")],
  ["rename onto the log", () => fs.renameSync("ok.txt", AUDIT)],
  ["write the log, other case", () => fs.writeFileSync(AUDIT.toUpperCase(), "")],
  ["read the log", () => fs.readFileSync(AUDIT, "utf8")],
], ["PermissionDenied"]);

// R14: the Forge namespace cannot be changed to swap a function.
replay("R14", "Forge namespace frozen", [
  ["assign Forge.exit", () => { Forge.exit = () => {}; return "assigned"; }],
  ["assign Forge.env.get", () => { Forge.env.get = () => "x"; return "assigned"; }],
  ["assign Forge.stdout.write", () => { Forge.stdout.write = () => 0; return "assigned"; }],
  ["define Forge.args", () => Object.defineProperty(Forge, "args", { value: [] })],
  ["delete Forge.version", () => { delete Forge.version; return "deleted"; }],
  ["push to Forge.args", () => Forge.args.push("x")],
  ["set prototype of Forge", () => Object.setPrototypeOf(Forge, {})],
], ["PermissionDenied", undefined]);

// R15: a hard link inside the grant to a file outside it, and a rename or
// copy whose destination is outside the grant.
replay("R15", "hard link and destinations", [
  ["write hard.txt", () => fs.writeFileSync("hard.txt", "x")],
  ["append hard.txt", () => fs.appendFileSync("hard.txt", "x")],
  ["rename to O", () => fs.renameSync("ok.txt", O + "\\moved.txt")],
  ["copy to O", () => fs.copyFileSync("ok.txt", O + "\\copied.txt")],
  ["copy to a stream", () => fs.copyFileSync("ok.txt", "ok.txt:hidden")],
]);

// The same refusals through the Promise-returning functions.
replayAsync("R16", "async functions refuse the same way", [
  ["readFile traversal", () => fs.readFile(G + "\\..\\outside\\secret.txt")],
  ["readFile bytes path", () => fs.readFile(ascii(SECRET))],
  ["readFile UNC", () => fs.readFile("\\\\192.0.2.1\\share\\x")],
  ["readFile device path", () => fs.readFile("\\\\?\\" + SECRET)],
  ["writeFile forge.json.", () => fs.writeFile("forge.json.", "x")],
  ["writeFile junction", () => fs.writeFile("out-link\\new.txt", "x")],
  ["readdir git-link", () => fs.readdir("git-link")],
  ["rm audit log", () => fs.rm(AUDIT)],
  ["copyFile to O", () => fs.copyFile("ok.txt", O + "\\copied.txt")],
]);

// A replay whose Promise never settles must fail loudly, not vanish.
const watchdog = setTimeout(() => {
  console.log("replay BYPASSED: a Promise-based replay did not settle within 10 seconds");
  Forge.exit(1);
}, 10000);

Promise.allSettled(pending).then(() => {
  clearTimeout(watchdog);
  console.log(`replays done: ${blocked} blocked, ${bypassed} bypassed`);
  if (bypassed > 0) {
    Forge.exitCode = 1;
  }
});
