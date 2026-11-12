// Compile-only checks for ../forge.d.ts (never run). CI runs
// `tsc -p types/test`; a line marked @ts-expect-error must stay an error.

const t: string = fs.readFileSync("a.txt", "utf8");
const b: Uint8Array = fs.readFileSync("a.txt");
const e: ForgeDirEntry[] = fs.readdirSync(".", { withFileTypes: true });
const n: string[] = fs.readdirSync(".");
async function f() { const s = await fs.stat("x"); return s.isFile && s.nlink; }
const id = setTimeout((a: number, s: string) => console.log(a, s), 10, 1, "x");
clearTimeout(id);
Forge.exitCode = 3;
const v: string | undefined = Forge.env.get("PATH");
Forge.addSignalListener("SIGINT", (s) => { Forge.stdout.write(s); });
try { fs.rmSync("d", { recursive: true }); } catch (err) { const fe = err as ForgeError; if (fe.code === "PermissionDenied") print(fe.permission); }
queueMicrotask(() => {});
// @ts-expect-error  Forge is frozen
Forge.version = "x";
// @ts-expect-error  wrong signal
Forge.addSignalListener("SIGTERM", () => {});
// @ts-expect-error  paths must be strings
fs.readFileSync(new Uint8Array(1));
function never(): number { Forge.exit(1); }
