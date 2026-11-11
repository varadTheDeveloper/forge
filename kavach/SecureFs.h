#pragma once

// Handle-based file operations (Phase 8.3, rule R3: "files are checked on the
// opened handle, hard links handled").
//
// Every function takes a kavach::CheckedPath, the proof that the path passed
// the checker, and then works on handles so the check cannot be undone by a
// change on disk between the check and the operation:
//
//   * The parent folder is opened first and its real location (the final
//     path Windows reports for the handle) must be exactly the folder that
//     was checked. The file itself is then opened *relative to that handle*
//     (NtCreateFile with a root directory), so swapping a folder for a
//     junction after the check cannot redirect the operation.
//   * Writing, creating, renaming and deleting never follow a symbolic link
//     or junction at the last step: the link itself is opened. Writing
//     through a link is refused; deleting a link deletes the link, and a
//     recursive delete never descends into one.
//   * Writing to a file with more than one hard link is refused, because
//     the same file may be reachable from outside the granted folders.
//   * Reading may follow links, but the handle's final path must still be
//     covered by a read grant and not be protected.
//
// A refusal returns ErrorCode::PermissionDenied and fills `denial`, so the
// caller can show Kavach's message. Every function is safe to call from a
// worker thread.

#if !defined(_WIN32)
#error "forge::kavach::fs: Windows-only for now."
#endif

#include "forge-core/Result.h"
#include "forge-core/Span.h"
#include "forge-core/String.h"
#include "forge-core/Types.h"
#include "forge-core/memory/ResultVoid.h"
#include "forge-core/memory/Vector.h"
#include "kavach/Kavach.h"

namespace forge::kavach::fs
{

enum class EntryType : uint8_t
{
    File,
    Directory,
    Symlink,  // a symbolic link or a junction
    Other,
};

struct Stat
{
    forge::core::u64 size = 0;
    EntryType type = EntryType::Other;
    forge::core::u32 links = 0;
    double mtimeMs = 0;      // last write
    double atimeMs = 0;      // last access
    double ctimeMs = 0;      // last change (metadata or contents)
    double birthtimeMs = 0;  // creation
    bool readOnly = false;
};

struct DirEntry
{
    forge::core::String name;
    EntryType type = EntryType::Other;
};

enum class WriteMode : uint8_t
{
    Truncate,  // create or replace the contents
    Append,    // create or add to the end
};

/// The whole file. Follows links; the real file must be readable.
[[nodiscard]]
forge::core::Result<forge::core::Vector<forge::core::u8>> ReadFile(
    const CheckedPath& path,
    Denial& denial) noexcept;

/// Creates the file if needed and writes `data`. Never follows a link at
/// the last step; refuses multiply hard-linked files.
[[nodiscard]]
forge::core::Result<void> WriteFile(
    const CheckedPath& path,
    forge::core::Span<const forge::core::u8> data,
    WriteMode mode,
    Denial& denial) noexcept;

/// False if nothing is there. A link whose target is outside the read
/// grants is refused rather than reported.
[[nodiscard]]
forge::core::Result<bool> Exists(
    const CheckedPath& path,
    Denial& denial) noexcept;

/// `followLinks` false describes a link itself (lstat).
[[nodiscard]]
forge::core::Result<Stat> GetStat(
    const CheckedPath& path,
    bool followLinks,
    Denial& denial) noexcept;

/// The real location, links followed (realpath).
[[nodiscard]]
forge::core::Result<forge::core::String> RealPath(
    const CheckedPath& path,
    Denial& denial) noexcept;

[[nodiscard]]
forge::core::Result<forge::core::Vector<DirEntry>> ReadDirectory(
    const CheckedPath& path,
    Denial& denial) noexcept;

/// Non-recursive: the parent must exist and the folder must not.
/// Recursive: creates every missing level and succeeds if it already exists.
[[nodiscard]]
forge::core::Result<void> MakeDirectory(
    const CheckedPath& path,
    bool recursive,
    Denial& denial) noexcept;

/// Removes a file, a link (never its target) or, with `recursive`, a folder
/// and everything in it. A folder without `recursive` fails with
/// InvalidOperation. Read-only files are removed too.
[[nodiscard]]
forge::core::Result<void> Remove(
    const CheckedPath& path,
    bool recursive,
    Denial& denial) noexcept;

/// Renames (moves) `from` to `to` on the same volume, replacing a file at
/// `to`. A link is renamed itself.
[[nodiscard]]
forge::core::Result<void> Rename(
    const CheckedPath& from,
    const CheckedPath& to,
    Denial& denial) noexcept;

/// Copies the contents of `from` (links followed, read-checked) to `to`
/// (created or replaced, same rules as WriteFile).
[[nodiscard]]
forge::core::Result<void> CopyFile(
    const CheckedPath& from,
    const CheckedPath& to,
    Denial& denial) noexcept;

} // namespace forge::kavach::fs
