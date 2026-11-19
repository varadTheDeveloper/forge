# Kavach: permissions in Forge

Kavach™ is the permission system of Forge™. This file describes what exists today
(Phase 9.4), how to use it, and the user-experience rules every later
permission feature must follow (gap G38 in the master plan).

Status: **preview**. Since Phase 9.4 every package runs isolated from the
program and holds only the permissions it declares and the program grants,
within what the command line allows. Until Forge runs inside an operating-system sandbox (Phase 13), Kavach
cannot stop an exploit of the JavaScript engine itself. See "Known limits"
below.

## Using it

Nothing outside the process is allowed unless you grant it on the command
line, before the script name:

```
forge --allow-read=data --allow-write=out --allow-env=HOME,LANG app.js
```

| Option | Grants |
|---|---|
| `--allow-read=<path>` | Reading files and folders at or under `<path>`. Repeat it for more paths. |
| `--allow-write=<path>` | Creating, changing and deleting at or under `<path>`. Repeat it for more paths. Write does not include read. |
| `--allow-env=<A,B,...>` | Reading these environment variables (names are case-insensitive on Windows). |
| `--allow-env` | Reading every environment variable, including `Forge.env.toObject()`. |
| `--audit-log=<file>` | Appending every permission decision to `<file>`, one JSON object per line. |

Relative paths are resolved against the folder Forge was started in.
Without any option a script can still compute, use timers, and write to the
console, standard output and standard error.

### What can never be granted

- **A whole drive or network share** (`C:\`, `\\server\share`) and **your
  whole home folder** (or a folder that contains it). Forge refuses to
  start with such an option and says why. Grant a folder inside it instead.
- **Protected paths** (rule R13): `.git` folders, files named `forge.json`
  or `forge.lock`, the audit log, and Forge's install folder. No grant
  covers them, even a grant of a folder that contains them.

### Paths Kavach refuses outright

Windows gives some path forms more than one meaning. Kavach refuses them
instead of guessing (the error has `code: "InvalidArgument"`):

- device and namespace paths: `\\?\...`, `\\.\...`, `\??\...`;
- drive-relative paths such as `C:file`;
- names ending in a dot or a space (Windows silently drops them);
- `:` inside a name (NTFS alternate data streams, `file.txt::$DATA`);
- reserved device names in any folder: `CON`, `PRN`, `AUX`, `NUL`,
  `COM0`-`COM9`, `LPT0`-`LPT9`, `CONIN$`, `CONOUT$`, with or without an
  extension;
- `< > " | ? *` and control characters;
- `..` that would go above the drive or share root;
- 8.3 short names (`PROGRA~1`) that do not exist on disk;
- anything that is not a string: a Uint8Array, a number, or an object with
  its own `toString()` (which is never called).

### How a path is checked

1. The path is put in canonical form (`kavach/PathRules.cpp`): absolute,
   `\` separators, no `.` or `..`, upper-case drive letter.
2. It must be covered by a grant **as written**. If not, it is refused
   without touching the disk, so a network share is never contacted (and
   never sent your Windows credentials) unless a grant names it.
3. It is resolved: symbolic links, junctions and short names are followed
   to the real location (`GetFinalPathNameByHandleW`).
4. The real location must also be covered by a grant, and must not be
   protected. A junction from inside a granted folder to somewhere else is
   therefore refused.

Every native function Forge exposes to JavaScript is listed, with the
permission it needs, in `kavach/Ops.h` (rule R2). `forge --self-test` fails
if a function exists without a row, or a row without a function. The web
APIs of Phase 10 (`DOMException`, `TextEncoder`, `TextDecoder`, `atob`,
`btoa`, `EventTarget`, `Event`, `reportError`, `self`, `navigator`, the
`on…` handlers, `AbortController`, `AbortSignal`, ...) have one row each
with no permission: none of them reaches outside the process
(PHASE10_DESIGN.md).

### Loading modules (Phase 9.1)

Loading a program's own code is not a filesystem permission (decision D23
in `PHASE9_DESIGN.md`). Forge finds the program's **module root**, the
nearest folder at or above the file you run that holds `forge.json`, or
else that file's folder, and the program may import modules only from
there. No option widens it, and `--allow-read` is neither needed nor used
for it. The home folder and a drive or share root are never module roots.

Phase 9.2: each package the program uses (a folder with forge.json named
in `"dependencies"`) becomes a module root too, the first time it is
imported. Only the program's own forge.json may name a folder outside the
program's folder. forge.json itself stays protected; the loader reads it
through a dedicated check that refuses a forge.json that is a link and
records the read in the audit log.

Phase 9.3: packages run isolated. Every dependency runs in a separate
compartment whose built-in objects and global object are frozen and which
has no `fs`, no `Forge` namespace and no `eval`; it gets only `console`,
the timers and `queueMicrotask`. Forge's own functions and objects, and
the global object, never cross into a package (a `TypeError`), and a
package never sees the program's built-in objects (it gets its own
instead), so it cannot pollute the program's prototypes. Details:
`PHASE9_DESIGN.md` section 5. `--frozen-intrinsics` freezes the program's
own built-in objects too.

### Permissions for packages (Phase 9.4)

A package declares what it needs in its forge.json, and the program grants
it in its own forge.json. Paths are relative to the program's folder:

```json
{ "name": "reader", "exports": "./index.js",
  "permissions": { "read": ["./data"], "env": ["HOME"] } }
```

```json
{ "dependencies": { "reader": "file:./vendor/reader" },
  "grants": { "reader": { "read": ["./data"], "env": ["HOME"] } } }
```

The package may then read under `./data` and the variable `HOME`, **if the
command line allows it too** (`--allow-read=...\data`, `--allow-env=HOME`):
the person running Forge always has the last word. A package that holds a
permission runs in a compartment of its own with `fs` and `Forge.env`
(no `Forge.exit`, `Forge.args` or signal listeners), and every call is
checked against that package's policy, whoever calls the package. A
package never reads every variable at once. A refusal names the package
and the layer that refused, and says what would allow it:

```
Permission denied: the package "reader" (fs.readFileSync) declares read access to
C:\app\private\x.txt, but the program's forge.json does not grant it.
To allow it, add to "grants" in the program's forge.json: "reader": { "read": ["C:\\app\\private\\x.txt"] }
```

The audit log records each package's declared and granted permissions
(`"event":"package"`) and adds `"package"` to every decision made for a
package's code; refusals give the reason `not-declared`,
`not-granted-by-program` or `not-allowed-by-command-line`.

Every module path goes through the same checks as `fs`: the path rules
above, the as-written check, link resolution and the protected paths, then
a handle-based read. Each decision is audited with op `import` and
permission `module`, so a junction from the program's folder to somewhere
else, or an import of a file in `.git`, is refused and recorded.

Options such as `{ recursive: true }` are read from the object's own data
properties only (Phase 8.4): a property inherited from a prototype is
ignored, so prototype pollution cannot make an operation do more, and an
options object with a getter, or a Proxy, is refused.

Kavach is tested against published bypasses of other runtimes' permission
systems on every build: see [BYPASS-REPLAYS.md](BYPASS-REPLAYS.md). Every
place where Forge's C++ runs script code is listed in
[CallSites.md](CallSites.md).

## Messages (permission UX, G38)

A refusal is an ordinary `Error` that the script can catch. Its message is
written for the person running the program, not for the author of the code:

```
Permission denied: fs.readFileSync needs read access to C:\work\notes.txt.
To allow it, run Forge with --allow-read=C:\work\notes.txt (or a folder that contains it).
```

The rules every message follows:

1. **Say what was refused**, by its JavaScript name, and **what it touched**,
   by the full real path (after links were followed) or variable name.
2. **Say why** in one plain sentence: not granted, protected, or not a
   usable path (with the specific reason).
3. **Give the exact option that would allow it**, ready to copy, and only
   when one exists. Protected paths and refused paths say that no option
   can allow them.
4. **No jargon** beyond the option names: no "capability", "ACL" or error
   numbers in the first line.
5. **Machine-readable fields** for programs: `code` (`PermissionDenied` or
   `InvalidArgument`), `permission` (`read`, `write`, `env`, `env (every
   variable)`, `module`), `op`, and `path` or `variable`.
6. **Startup refusals** (an option Forge will not accept) exit with code 2
   before any script runs, and name the option exactly as typed.

### Rules for the approval screens (Phase 12)

When packages declare permissions and a person approves them at install or
update time, the approval screen must follow these rules, in addition to
the six above:

- **Silent when nothing is needed.** A package that asks for no permission
  installs without a prompt.
- **Only what is new.** An update shows only permissions the previous
  approved version did not have.
- **Ranked by risk**, highest first: running programs and writing outside
  the project; network access to arbitrary hosts; reading secrets
  (environment, home folder); writing inside the project; reading inside
  the project.
- **The top tier needs a typed confirmation** (typing the package name),
  never a single key press, and never from a script or an AI agent (R16).
- **Plain language and examples**: "can read any file in your home folder",
  not `read:~/**`.
- **Accessible and translatable**: no meaning carried by colour alone; all
  text in one message catalogue so it can be translated.

## Audit log

With `--audit-log=<file>`, Forge appends one JSON object per line:

```json
{"time":"2026-11-10T19:30:00.123Z","event":"start","pid":4242,"read":["C:\\work\\data"],"write":[],"env":["HOME"]}
{"time":"2026-11-10T19:30:00.130Z","op":"fs.readFileSync","permission":"read","target":"C:\\work\\data\\a.txt","decision":"allow"}
{"time":"2026-11-10T19:30:00.131Z","op":"fs.readFileSync","permission":"read","target":"C:\\work\\secret.txt","decision":"deny","reason":"not-granted"}
{"time":"2026-11-10T19:30:01.000Z","op":"fs.readFileSync","suppressed":{"allow":5120,"deny":0}}
{"time":"2026-11-10T19:30:02.000Z","event":"stop"}
```

- Every decision is recorded, allowed and refused, written by C++ before
  the operation runs. Reasons: `not-granted`, `protected`, `invalid-path`,
  `audit-failed`, `internal-error`.
- **Rate limit (R15):** each operation may write a burst of 1,000 records
  and then 100 per second. Decisions beyond that are still made and
  enforced; they are counted and written as one `suppressed` record, so a
  loop cannot fill the disk while the log still accounts for everything.
- **Fail closed (R7):** if a record cannot be written, the operation is
  refused, and so is every later one.
- The log file is opened for appending only and is protected: no grant
  covers it.

## On disk: handles, links and hard links (Phase 8.3)

The check above decides on names. Every file operation then works on
handles (`kavach/SecureFs.cpp`), so a change on disk between the check and
the operation cannot widen what it reaches:

- The parent folder is opened and must still be exactly the folder that was
  checked; the file is then opened **relative to that folder's handle**.
  Replacing a folder with a junction after the check is detected.
- Reads may follow links, but the opened file's real location must still
  be covered by a read grant and not be protected.
- Writing, creating, renaming and deleting never follow a link at the last
  step. Writing *through* a symbolic link or junction is refused; deleting
  one deletes the link itself; a recursive delete never descends into one.
- Writing a file that has **more than one hard link** is refused, because
  the same file may also be reachable from outside the granted folders.
  Reading such a file is allowed (package stores commonly use hard links).

Refusals found at this stage say so in plain words (`"... found that the
path led to ..."`, `"... will not write through ..."`, `"... has more than
one hard link ..."`) and are audited with the reasons `changed-after-check`,
`link` and `hard-link`.

## Known limits

- **Grants are by name.** Until `forge.lock` binds a grant to a package's
  identity (Phase 14), a grant naming a package that two folders hold is
  refused.
- **Delegation by code.** A closure the program (or a package) passes to
  another package can still use the giver's access: that is the giver's
  decision. Forge's own functions themselves never cross.
- **Pure packages share one compartment.** They cannot change anything
  they share (it is frozen), but one can read the last RegExp match
  (`RegExp.lastMatch`) of another (`PHASE9_DESIGN.md`, R19).
- **Creating links** (`symlink`, `link`) is not offered yet.
- **Non-ASCII letter case.** Matching ignores the case of ASCII letters
  only. A path whose non-ASCII letters differ in case from the grant, in a
  part that does not exist yet, is refused (never wrongly allowed).
