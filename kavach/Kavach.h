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

    /// Moves the path out, for an operation that runs on a worker thread
    /// after the check (the async fs functions). Leaves this proof empty.
    forge::core::Path ReleasePath() noexcept { return std::move(path_); }

private:
    friend class Checker;

    forge::core::Path path_;
    forge::core::String text_;
    Op op_ = Op::Count;
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
    };

    Kind kind = Kind::None;
    PathError pathError = PathError::None;

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
