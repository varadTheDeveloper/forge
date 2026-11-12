# Bypass replays

Permission systems in other JavaScript runtimes have been bypassed in
public. This file lists the bypasses Forge replays against Kavach on every
build, together with Windows path tricks that give one path more than one
meaning. The replays live in `tests/bypass-replays.js` and run as a
runtime test (`tests/run-runtime-tests.ps1`). The runner also checks from
outside that nothing outside the granted folder, and none of the protected
files, changed.

A replay passes when the operation is **refused** with `PermissionDenied`
or `InvalidArgument`. Any other outcome is printed as `BYPASSED` and fails
the build.

When a new bypass is published for Node, Deno, Bun or another runtime with
permissions, add a replay here in the same change that fixes Forge (or that
shows Forge was not affected).

## The setup

The runner makes a temporary tree and runs Forge with
`--allow-read=G --allow-write=G --allow-env=FORGE_REPLAY_GRANTED
--audit-log=G\audit.jsonl`, with `G` as the current folder:

| Path | What it is |
|---|---|
| `G\` | the granted folder |
| `G\.git\config`, `G\forge.json` | protected files (rule R13) |
| `G\audit.jsonl` | the audit log, inside the grant but protected |
| `G\out-link` | a junction to `O\` |
| `G\git-link` | a junction to `G\.git` |
| `G\hard.txt` | a hard link to `O\secret.txt` |
| `G\victim\keep.txt` | a folder that must survive |
| `O\secret.txt` | outside every grant |

## The replays

| ID | Replay | Origin | What Forge does |
|---|---|---|---|
| R01 | `..` in every spelling (`G\..\O`, `G/victim/../../O`, relative `..`, doubled and mixed separators) | Node [CVE-2023-30584](https://nvd.nist.gov/vuln/detail/cve-2023-30584): path traversal past the permission model | The path is made canonical before any decision; the result is outside the grant. |
| R02 | The path as bytes: `Uint8Array`, `ArrayBuffer`, `DataView` | Node [CVE-2023-32004](https://ubuntu.com/security/CVE-2023-32004) (Buffer paths) and [CVE-2023-39332](https://nvd.nist.gov/vuln/detail/CVE-2023-39332) (Uint8Array paths) skipped the traversal check | Only a string is a path (rule R3). Anything else is `InvalidArgument`. |
| R03 | A path object whose `toString` returns a granted path the first time and another path later; `Symbol.toPrimitive`; `new String()`; `[path]` | Check-one-use-another (time of check, time of use) through conversion | Never converted, so the object never chooses its path. The replay also checks `toString` was not called. |
| R04 | Network paths `\\192.0.2.1\share`, `//192.0.2.1/share`, `\\forge-replay.invalid\share` | Windows sends the user's NTLM credentials to any SMB server a program connects to | Refused on the path as written, before the disk or network is touched. The replay requires an answer in under 3 seconds (a connection attempt to that unroutable address takes far longer). |
| R05 | Device and namespace paths: `\\?\`, `\\.\`, `\??\`, `\\?\GLOBALROOT\Device\...`, `\\.\PhysicalDrive0`, `\\.\pipe\`, `CON`, `NUL.txt`, `COM1`, `CONIN$`, drive-relative `C:file` | Deno [CVE-2024-34346](https://github.com/denoland/deno/security/advisories/GHSA-23rx-c3g5-hv9w): device files such as `/proc/self/mem` gave more than the grant. These are the Windows equivalents. | Refused as ambiguous, even when they name a granted file. |
| R06 | 8.3 short names: `GIT~1\config`, `FORGE~1.JSO`, `AUDIT~1.JSO` for read, write, mkdir, rm and rename | Short names are a second name for the same file | A short name that exists is resolved to the long name, which is protected; one that does not exist is refused. `rm` and `rename`, which do not follow the last component, refuse a short-name last component. |
| R07 | Other spellings of protected names: `forge.json.`, `"forge.json "`, `FORGE.JSON`, `forge.json::$DATA`, `forge.json:stream`, `victim\..\forge.json`, `.GIT`, `.git.`, a git hook, rename or copy onto a protected name | Windows drops trailing dots and spaces, ignores case, and `::$DATA` is the file's main stream | Trailing dots and spaces and `:` are refused; case is ignored when matching. |
| R08 | A junction inside the grant that points outside it: read, stat, readdir, realpath, exists, write, append, mkdir, rename and copy out of it, rm through it | Link following | Every path is checked again after links are followed, and file operations work on handles opened relative to the checked folder (Phase 8.3). |
| R09 | A junction inside the grant that points at `.git` | Link to a protected path | The real location is protected. |
| R10 | `Object.prototype.recursive = true` before `rmSync(folder, {})` | Prototype pollution changing an option | Options are read from the object's own data properties only (Phase 8.4). |
| R11 | `process`, `require`, `module`, `Deno`, `Bun`, `fetch`, `Buffer`, ... and `import("node:fs")` | Ambient authority from another runtime's API | None of them exist. `import()` is rejected with `code: "NotSupported"` until modules arrive (Phase 9). The first run of this replay found that the Promise never settled, because no module load hook was installed; Forge now installs one that refuses every request. A 10-second watchdog fails the run if any Promise-based replay never settles. |
| R12 | Environment names with NUL or `=`, the hidden `=C:` variables, an empty name, `toObject()` with a list grant | Widening a per-variable grant | Names are checked one by one; invalid names are refused. The granted name still works, in any case. |
| R13 | Writing, appending, deleting, renaming, reading the audit log, and writing it under another letter case | A script erasing the record of what it did | The audit log is protected even inside a granted folder. |
| R14 | Replacing `Forge.exit`, `Forge.env.get`, `Forge.stdout.write`, redefining `Forge.args`, changing Forge's prototype | Swapping a function another part of the program relies on | The namespace and its sub-objects are frozen. |
| R15 | Writing a hard link to an outside file; rename and copy to a destination outside the grant or to a stream | Hard links reach the same file from two places | Writing a file with more than one hard link is refused; both ends of rename and copy are checked. |
| R16 | R01, R02, R04, R05, R07, R08, R09, R13 and R15 again through the Promise-returning functions | Async paths sometimes skip checks | The same check runs before the work is queued; a refusal is a rejected Promise. |

## Not replayed yet

- **Racing a link swap** between the check and the use. Phase 8.3 closed
  this by design (handles opened relative to the checked folder, and the
  opened path checked again), but a replay needs a second process that
  swaps a folder for a junction in a loop. Planned with the Phase 9 test
  helpers.
- **Per-package bypasses** (one package using another's grant). They need
  per-package permissions (Phase 9).
- **Engine exploits.** Kavach cannot stop a bug in the JavaScript engine
  itself; that needs the operating-system sandbox of Phase 13.
