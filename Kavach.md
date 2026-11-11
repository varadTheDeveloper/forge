# Kavach: permissions in Forge

Kavach is Forge's permission system. This file describes what exists today
(Phase 8.3), how to use it, and the user-experience rules every later
permission feature must follow (gap G38 in the master plan).

Status: **preview**. Until packages are isolated from each other (Phase 9)
and Forge runs inside an operating-system sandbox (Phase 13), Kavach limits
what a whole program can do; it does not yet stop one package from using
another package's access, and it cannot stop an exploit of the JavaScript
engine itself. See "Known limits" below.

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
if a function exists without a row, or a row without a function.

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
   variable)`), `op`, and `path` or `variable`.
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

- **One policy for the whole program.** Per-package permissions need
  isolated packages (Phase 9).
- **Creating links** (`symlink`, `link`) is not offered yet.
- **Non-ASCII letter case.** Matching ignores the case of ASCII letters
  only. A path whose non-ASCII letters differ in case from the grant, in a
  part that does not exist yet, is refused (never wrongly allowed).
