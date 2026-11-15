# Per-package permission test tree (Phase 9.4)

`run-runtime-tests.ps1` copies `app/` into a temporary folder, adds
`data/note.txt`, `private/secret.txt` and `other/x.txt`, and runs
`app/src/main.js` with `--allow-read` for `data` and `private`,
`--allow-env=FORGE_PERM_TEST,FORGE_PERM_OTHER` (both set) and an audit log.
The program's forge.json grants `reader` `./data` and `FORGE_PERM_TEST`,
`greedy` `./data`, and `capped` `./other`.

| Package | Declares | What it tests |
|---|---|---|
| `reader` | read `./data`, env `FORGE_PERM_TEST` | Its own compartment with `fs` and `Forge.env` (no `Forge.exit`, no `Forge.args`); reads of `./data` work; the private file, a write, another variable and `Forge.env.toObject()` are refused as not declared, even though the program itself may read the private file (attribution by realm, D24); the same through `fs.readFile` (async) and through a WebAssembly import; its `fs` function refused at the boundary (confused deputy). |
| `greedy` | read `./data`, `./private` | Granted only `./data`: the private file is refused as not granted; `reader`'s `fs` function does not reach it either. |
| `capped` | read `./other` | Granted by forge.json but not allowed on the command line: refused, naming the command line. |
| `wanting` | read `./data` | Granted nothing: runs in the shared compartment without `fs` or `Forge`, and Forge warns once. |

The runner also checks the audit log (`"package"` records and the three
refusal reasons) and builds a small program whose grant names a package
held by two folders, which is refused.
