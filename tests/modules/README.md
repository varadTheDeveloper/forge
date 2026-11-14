# Module test tree (Phase 9.1)

`run-runtime-tests.ps1` copies `app/` into a temporary folder, then adds
what cannot live in the repository: a junction `app/out-link` to a folder
outside the module root, a `.git` folder holding a module, and files next
to `app/` that must never load. It runs `app/src/main.js` with no
permissions at all: loading a program's own modules needs none (D23).

`app/forge.json` makes `app/` the module root, so `src/main.js` may import
`../shared/util.js` but not `../../outside.js`.
