# Package test tree (Phase 9.2)

`run-runtime-tests.ps1` copies `app/` into a temporary folder and adds a
package outside it (`outside-pkg/`) and a program with an invalid
forge.json (`bad-app/`). It runs `app/src/main.js` with no permissions.

| Package | Declared by | What it tests |
|---|---|---|
| `greet` | the app | exports map: `.`, conditions (`forge` wins in object order), a `*` pattern, a `null` (private) entry, a self-reference (`greet/extra`), a module that tries to import a file of the app by a relative path |
| `@forge-test/util` | the app and `greet` | a scoped name, a string `exports`, one instance shared by both |
| `outside` | the app, at `../outside-pkg` | only the program's own forge.json may name a folder outside it |
| `escaper` | the app | a dependency naming a folder outside the program (refused) |
| `sneaky` | nobody | an undeclared package cannot be imported |
| `broken` | the app | a forge.json with a duplicate key (refused) |
| `misnamed` | the app, pointing at `greet` | the folder's package name must match |
| `phantom-user` | the app | a package importing one it did not declare (refused) |
| `no-exports` | the app | a package without `exports` exposes nothing |
