#pragma once

// Kavach path rules (Phase 8.2, rule R3): the one function that turns a path
// written by a script into the canonical form every permission check uses,
// plus the matcher that decides whether a grant covers a path.
//
// Everything here is pure string work with Windows path semantics and no
// operating-system calls, so it builds and runs on Linux too, where the
// sanitizer CI job and the fuzzer test it (R24). Resolving symbolic links,
// junctions and 8.3 short names needs the file system; that is done on
// Windows by kavach/Resolve.cpp, on top of the output of this file.
//
// Canonical form:
//   C:\dir\file            drive letter upper case, '\' separators
//   C:\                    a drive root (the only form ending in '\')
//   \\server\share\dir     UNC
// No ".", "..", empty components, '/' or trailing separator.

#include "forge-core/String.h"
#include "forge-core/StringView.h"
#include "forge-core/Types.h"

namespace forge::kavach
{

/// Why a path was rejected before any permission was even considered.
/// These forms are refused because Windows gives them more than one meaning
/// or because they reach things other than ordinary files (R22).
enum class PathError
{
    None,
    Empty,
    ContainsNul,
    InvalidCharacter,    // < > " | ? * or a control character
    DevicePath,          // \\?\..., \\.\..., \??\...
    DriveRelative,       // "C:file" (relative to a per-drive directory)
    InvalidUnc,          // "\\server" without a share, "\\\x"
    EscapesRoot,         // ".." above the drive or share root
    TrailingDotOrSpace,  // "file." or "file " (Windows drops them silently)
    AlternateStream,     // "file:stream" (NTFS alternate data streams)
    ReservedName,        // CON, NUL, COM1, LPT1, ... with or without extension
    TooLong,
    ShortName,           // "PROGRA~1" that is not on disk (set by the resolver)
};

/// A short, plain-language reason, for error messages.
const char* DescribePathError(
    PathError error) noexcept;

struct CanonicalPath
{
    forge::core::String text;

    /// Number of path components after the root ("C:\a\b" has 2).
    forge::core::Size componentCount = 0;

    /// 1-based index of the last component that looks like an 8.3 short
    /// name ("PROGRA~1"), or 0 if there is none. Such a component is only
    /// acceptable if the resolver found it on disk and replaced it with the
    /// long name; see kavach/Resolve.cpp.
    forge::core::Size lastShortNameComponent = 0;

    bool isUnc = false;
};

/// Canonicalizes `input` (UTF-8, '/' or '\'). A relative path is taken
/// relative to `cwd`, and a root-relative one ("\dir") to `cwd`'s root.
/// `cwd` must itself be canonical. On error `out` is left empty.
[[nodiscard]]
PathError LexicalCanonicalize(
    forge::core::StringView input,
    forge::core::StringView cwd,
    CanonicalPath& out) noexcept;

/// Compares two canonical paths, ignoring ASCII case. Non-ASCII letters
/// must match exactly; the resolver gives paths that exist their on-disk
/// spelling, so a difference there can only make a check fail, never pass.
[[nodiscard]]
bool PathEquals(
    forge::core::StringView a,
    forge::core::StringView b) noexcept;

/// True if `path` is `grant` or lies inside it. Both must be canonical.
/// "C:\work" covers "C:\work\a" but not "C:\workshop".
[[nodiscard]]
bool PathCovers(
    forge::core::StringView grant,
    forge::core::StringView path) noexcept;

/// True for "C:\" and "\\server\share".
[[nodiscard]]
bool IsRootPath(
    forge::core::StringView canonical) noexcept;

/// True if any component of `canonical` equals `name` (ASCII case ignored).
[[nodiscard]]
bool HasComponent(
    forge::core::StringView canonical,
    forge::core::StringView name) noexcept;

/// The last component ("file.txt" for "C:\a\file.txt"); empty for a root.
[[nodiscard]]
forge::core::StringView LastComponent(
    forge::core::StringView canonical) noexcept;

/// Everything before the last component, without the separator ("C:\a"
/// for "C:\a\b", "C:\" for "C:\a"); the path itself for a root.
[[nodiscard]]
forge::core::StringView ParentPath(
    forge::core::StringView canonical) noexcept;

} // namespace forge::kavach
