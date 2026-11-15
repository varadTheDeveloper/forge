#pragma once

// Kavach, the permission checker (Phase 8.2).
//
// Rules this file implements (FORGE_MASTER_PLAN_v2.md, section 4):
//   R1  The checker is C++ that JavaScript cannot reach or change.
//   R2  Deny by default; every native is in the op table (kavach/Ops.h).
//   R3  One canonical path function (kavach/PathRules.h); only strings are
//       accepted as paths; symbolic links, junctions and 8.3 names are
//       resolved before matching; a network path is never opened unless a
//       grant covers it as written (no credential leak on open).
//   R7  Fail closed: an invalid grant stops startup; a failed audit write
//       refuses the operation.
//   R13 Protected paths: no grant ever covers a .git folder, forge.json,
//       forge.lock, the audit log or Forge's install folder; a grant of a
//       drive root, a share root or the home folder is refused.
//
// Until packages exist (Phase 9) there is one policy, for the whole
// program, built once from the command line before any script runs.
// It never changes afterwards.
//
// Known limits of this step (documented in HISTORY.md, "Phase 8.2"):
//   * Paths are checked, then opened by name. A link swapped in between can
//     still redirect the operation (a race). Handle-based checks come with
//     the full filesystem API (8.3).
//   * A hard link inside a granted folder to a file outside it is not
//     detected yet (also 8.3).

#if !defined(_WIN32)
#error "forge::kavach: the checker is Windows-only for now; kavach/PathRules is portable."
#endif

#include <utility>

#include "forge-core/Path.h"
#include "forge-core/Result.h"
#include "forge-core/String.h"
#include "forge-core/StringView.h"
#include "forge-core/memory/ResultVoid.h"
#include "forge-core/memory/Vector.h"
#include "kavach/Ops.h"
#include "kavach/PathRules.h"

namespace forge::kavach
{

struct PolicyOptions
{
    /// --allow-read / --allow-write values, as typed.
    forge::core::Vector<forge::core::String> readPaths;
    forge::core::Vector<forge::core::String> writePaths;

    /// --allow-env=A,B names, or every variable when allEnv is set.
    forge::core::Vector<forge::core::String> envNames;
    bool allEnv = false;

    /// --audit-log path; empty for no audit log.
    forge::core::String auditLogPath;

    /// Overrides for tests; empty means "ask Windows".
    forge::core::String cwd;
    forge::core::String installDir;
    forge::core::String homeDir;
};

/// Builds the process policy and opens the audit log. Call exactly once,
/// before any script runs. On failure `message` explains which option is
/// wrong, in plain language, and the program must not start.
[[nodiscard]]
bool Initialize(
    const PolicyOptions& options,
    forge::core::String& message) noexcept;

/// Phase 9.1 (D23): adds a module root, the folder (or single file) the
/// program may load modules from. Loading code is not a filesystem
/// permission: --allow-read neither adds nor is needed for module roots,
/// and only Op::ModuleImport checks against them. Call after Initialize and
/// before any module loads. The same refusals as for grants apply (a drive
/// or share root, the home folder, a protected path); on failure `message`
/// explains why.
[[nodiscard]]
bool AddModuleRoot(
    forge::core::StringView path,
    forge::core::String& message) noexcept;

/// Phase 9.1: sets up module loading for the program `entry` (the script
/// argument, as typed). The module root is the nearest folder at or above
/// the entry that contains forge.json, else the entry's folder; never the
/// home folder or a drive or share root (then the entry file alone is the
/// root, PHASE9_DESIGN.md section 3). `entryResolved` receives the entry's
/// canonical, resolved path. On failure `message` explains why, in plain
/// language, and the program must not start.
[[nodiscard]]
bool SetUpProgramModules(
    forge::core::StringView entry,
    forge::core::String& entryResolved,
    forge::core::String& message) noexcept;

/// Phase 9.2: resolves the folder of a "file:" dependency. `path` is as
/// written in forge.json, relative to `baseFolder` (the declaring package's
/// root) unless absolute. The folder must exist and hold forge.json. On
/// success `resolved` is canonical with links followed; on failure
/// `message` says why.
[[nodiscard]]
bool ResolvePackageFolder(
    forge::core::StringView path,
    forge::core::StringView baseFolder,
    forge::core::String& resolved,
    forge::core::String& message) noexcept;

/// Phase 9.2: the root of the innermost package strictly inside `outerRoot`
/// that contains `path` (the nearest folder above `path`, below
/// `outerRoot`, that holds forge.json), or an empty string if `path`
/// belongs to `outerRoot` itself. Both are canonical, resolved paths.
void FindInnerPackageRoot(
    forge::core::StringView outerRoot,
    forge::core::StringView path,
    forge::core::String& inner) noexcept;

/// The first module root, canonical and resolved, for messages; empty when
/// there is none.
forge::core::StringView FirstModuleRoot() noexcept;

/// Writes any pending audit summary and closes the audit log.
void Shutdown() noexcept;

/// Proof that a path was checked for one op. The filesystem natives accept
/// only this, never a raw string, so every file operation has passed the
/// checker. It can only be made by CheckPath.
class CheckedPath
{
public:
    CheckedPath() = default;

    /// The resolved, canonical path, in forge-core's form (for File).
    const forge::core::Path& Path() const noexcept { return path_; }

    /// The same path in Windows form ("C:\dir\file"), for messages.
    forge::core::StringView Text() const noexcept { return text_.View(); }

    Op GetOp() const noexcept { return op_; }

    /// What the path was checked for (Phase 8.3).
    Access GetAccess() const noexcept { return access_; }

    /// Moves the path out, for an operation that runs on a worker thread
    /// after the check (the async fs functions). Leaves this proof empty.
    forge::core::Path ReleasePath() noexcept { return std::move(path_); }

    /// Phase 9.2: true for a package manifest read by the module loader
    /// (CheckManifest), the one case where a protected file is opened.
    bool IsManifest() const noexcept { return manifest_; }

private:
    friend class Checker;

    forge::core::Path path_;
    forge::core::String text_;
    Op op_ = Op::Count;
    Access access_ = Access::Read;
    bool manifest_ = false;
};

/// Why an operation was refused; turned into a message by FormatDenial.
struct Denial
{
    enum class Kind
    {
        None,
        InvalidPath,   // the path itself is not acceptable (PathError)
        NotGranted,    // no grant covers it
        Protected,     // R13: no grant can cover it
        AuditFailed,   // the decision could not be recorded (R7)
        Internal,      // out of memory or an unexpected OS failure
        Link,          // 8.3: writing or creating through a symbolic link or junction
        HardLink,      // 8.3: writing a file that has more than one hard link
        Changed,       // 8.3: the path led somewhere else when it was opened
    };

    Kind kind = Kind::None;
    PathError pathError = PathError::None;
    Access access = Access::Read;

    /// The canonical path (or the variable name); the input as typed when
    /// it could not be canonicalized.
    forge::core::String target;
};

/// Checks that `op` may use `path` (UTF-8, as the script wrote it). On
/// success returns the proof; on failure fills `denial` and returns
/// PermissionDenied (or InvalidArgument for an unacceptable path).
[[nodiscard]]
forge::core::Result<CheckedPath> CheckPath(
    Op op,
    forge::core::StringView path,
    Denial& denial) noexcept;

/// The same, for an op whose permission covers more than one kind of
/// access (fs.copyFile reads its source and writes its destination).
[[nodiscard]]
forge::core::Result<CheckedPath> CheckPath(
    Op op,
    Access access,
    forge::core::StringView path,
    Denial& denial) noexcept;

/// Phase 8.3, rule R3 ("checked on the opened handle"): called with the
/// final path of a handle that was opened for `checked`, as Windows reports
/// it (canonical form). True if it is still covered by a grant for the same
/// access and not protected. On false, `denial` says why (Kind::Changed or
/// Kind::Protected) and the refusal is audited. Safe to call from any thread.
[[nodiscard]]
bool VerifyOpenedPath(
    const CheckedPath& checked,
    forge::core::StringView finalPath,
    Denial& denial) noexcept;

/// Phase 9.2: proof for reading `packageRoot`\forge.json, the package
/// manifest. forge.json is a protected path (R13) that no grant covers;
/// only the module loader reads it, through this check. `packageRoot` must
/// already be a module root, and forge.json must be a plain file in it (not
/// a link). The read is audited as op `import`.
[[nodiscard]]
forge::core::Result<CheckedPath> CheckManifest(
    forge::core::StringView packageRoot,
    Denial& denial) noexcept;

/// Records a refusal decided after the check (Link, HardLink, Changed).
/// Safe to call from any thread.
void RecordLateDenial(
    const CheckedPath& checked,
    Denial& denial,
    Denial::Kind kind,
    forge::core::StringView target) noexcept;

/// Checks that `op` may read the environment variable `name`.
[[nodiscard]]
forge::core::Result<void> CheckEnv(
    Op op,
    forge::core::StringView name,
    Denial& denial) noexcept;

/// Checks that `op` may read every environment variable.
[[nodiscard]]
forge::core::Result<void> CheckEnvAll(
    Op op,
    Denial& denial) noexcept;

/// A plain-language message for a refusal (permission UX spec, G38):
/// what was refused, why, and the exact option that would allow it.
void FormatDenial(
    Op op,
    const Denial& denial,
    forge::core::String& message) noexcept;

/// The grants in effect, one per line, for `forge --help`-style output
/// and the self-test. Empty lines are never produced.
void DescribePolicy(
    forge::core::String& out) noexcept;

} // namespace forge::kavach
