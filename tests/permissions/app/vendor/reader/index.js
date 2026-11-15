// Phase 9.4: declares read access to ./data and the variable FORGE_PERM_TEST,
// and the program grants both, so it runs in a compartment of its own with
// fs and Forge.env bound to its own policy. Errors are not caught here: the
// program inspects them.

export function globals() {
  return [typeof fs, typeof Forge, typeof Forge.env, typeof Forge.exit, typeof Forge.args].join(" ");
}

export function read(path) {
  return fs.readFileSync(path, "utf8");
}

export function write(path) {
  fs.writeFileSync(path, "written by reader");
  return "written";
}

export function env(name) {
  return Forge.env.get(name);
}

export function envAll() {
  return Object.keys(Forge.env.toObject()).length;
}

export function readAsync(path) {
  return fs.readFile(path, "utf8");
}

// The confused-deputy attempt: handing out its own Forge function.
export function leakFs() {
  return fs.readFileSync;
}

// A WebAssembly module whose import is a function of this package: the
// read still runs with this package's policy, whoever calls the export.
// (module (import "env" "read" (func (param externref) (result externref)))
//         (func (export "read") (param externref) (result externref)
//           local.get 0 call 0))
const wasmBytes = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0,
  1, 6, 1, 96, 1, 111, 1, 111,
  2, 12, 1, 3, 101, 110, 118, 4, 114, 101, 97, 100, 0, 0,
  3, 2, 1, 0,
  7, 8, 1, 4, 114, 101, 97, 100, 0, 1,
  10, 8, 1, 6, 0, 32, 0, 16, 0, 11,
]);
let wasmReadExport = null;

export function wasmRead(path) {
  if (wasmReadExport === null) {
    const module = new WebAssembly.Module(wasmBytes);
    const instance = new WebAssembly.Instance(module, {
      env: { read: (p) => fs.readFileSync(p, "utf8") },
    });
    wasmReadExport = instance.exports.read;
  }

  return wasmReadExport(path);
}
