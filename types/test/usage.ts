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
try { fs.readFileSync("x"); } catch (err) { const fe = err as ForgeError; const who: string | undefined = fe.package; print(who ?? "the program"); }
queueMicrotask(() => {});
// @ts-expect-error  Forge is frozen
Forge.version = "x";
// @ts-expect-error  wrong signal
Forge.addSignalListener("SIGTERM", () => {});
// @ts-expect-error  paths must be strings
fs.readFileSync(new Uint8Array(1));
function never(): number { Forge.exit(1); }

// Phase 10.2: events.
const target = new EventTarget();
target.addEventListener("x", (ev: Event) => ev.preventDefault(), { once: true, passive: false });
target.addEventListener("x", { handleEvent(ev: Event) { ev.stopPropagation(); } }, true);
const delivered: boolean = target.dispatchEvent(new CustomEvent<number>("x", { detail: 1 }));
const detail: number = new CustomEvent<number>("x", { detail: 2 }).detail;
addEventListener("error", (ev) => { const m: string = ev.message; ev.preventDefault(); void m; });
addEventListener("unhandledrejection", (ev) => { const r: unknown = ev.reason; void r; });
addEventListener("custom", (ev: Event) => { void ev.timeStamp; });
onerror = (message, source, lineno, colno, error) => true;
onunhandledrejection = (ev) => { void ev.promise; return false; };
reportError(new Error("x"));
const ua: string = navigator.userAgent;
const me: typeof globalThis = self;
const phase: number = Event.AT_TARGET;
void delivered; void detail; void ua; void me; void phase;
// @ts-expect-error  dispatchEvent needs an Event
target.dispatchEvent({ type: "x" });

// Phase 10.3: AbortController and AbortSignal.
const controller = new AbortController();
target.addEventListener("x", () => {}, { signal: controller.signal });
const combined: AbortSignal = AbortSignal.any([controller.signal, AbortSignal.timeout(1000)]);
combined.onabort = (ev: Event) => { const why: unknown = combined.reason; void why; void ev; };
combined.addEventListener("abort", (ev) => { void ev.isTrusted; });
controller.abort(new Error("stop"));
const wasAborted: boolean = AbortSignal.abort().aborted;
void wasAborted;
controller.signal.throwIfAborted();
// @ts-expect-error  AbortSignal has no constructor
new AbortSignal();
// @ts-expect-error  the signal option takes an AbortSignal
target.addEventListener("x", () => {}, { signal: controller });
// @ts-expect-error  PromiseRejectionEvent needs its dictionary
new PromiseRejectionEvent("x");
