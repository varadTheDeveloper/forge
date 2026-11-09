#pragma once

#include "String.h"
#include "StringView.h"
#include "Types.h"
#include "memory/Vector.h"

namespace forge::core
{

/// A filesystem path, stored as a `String` (UTF-8, forward slashes
/// normalized internally regardless of what separator the input used).
/// Pure value manipulation only — see `Path.md` for the full spec.
/// `Path` never touches the OS; `File` (see `File.md`) is what actually
/// performs I/O against a location a `Path` describes.
class [[nodiscard]] Path
{
public:

    using SizeType = forge::core::Size;

    Path() noexcept;

    explicit Path(
        memory::Allocator& allocator) noexcept;

    /// `text` may use `/` or `\` as a separator; both are accepted and
    /// normalized to `/` internally. Does not collapse repeated
    /// separators or resolve `.`/`..` — that's Normalize()'s job.
    [[nodiscard]]
    static Result<Path> Create(
        memory::Allocator& allocator,
        StringView text);

    [[nodiscard]]
    static Result<Path> Create(
        StringView text);

    [[nodiscard]]
    StringView View() const noexcept;

    [[nodiscard]]
    bool Empty() const noexcept;

    /// True for a Windows drive-absolute path ("C:/...") or a UNC path
    /// ("//server/share..."). A bare leading "/" alone is NOT considered
    /// absolute (matches std::filesystem::path's own behavior on
    /// Windows: it has a root-directory but no root-name).
    [[nodiscard]]
    bool IsAbsolute() const noexcept;

    [[nodiscard]]
    bool IsRelative() const noexcept;

    /// Parent directory, e.g. "a/b/c.txt" -> "a/b". A root ("/", "C:/")
    /// is its own parent. Empty if there is no separator at all.
    [[nodiscard]]
    StringView Parent() const noexcept;

    /// Final path component, e.g. "a/b/c.txt" -> "c.txt". A single
    /// trailing separator is ignored for this purpose (so "a/b/" also
    /// yields "b" — a deliberate, documented simplification vs.
    /// std::filesystem::path, which would report an empty filename
    /// there).
    [[nodiscard]]
    StringView FileName() const noexcept;

    /// FileName() without its extension, e.g. "c.txt" -> "c". A dotfile
    /// with no other dot (".gitignore") has no extension, so Stem()
    /// returns the whole name.
    [[nodiscard]]
    StringView Stem() const noexcept;

    /// Extension including the leading dot, e.g. "c.txt" -> ".txt".
    /// Empty if FileName() has no (non-leading) dot.
    [[nodiscard]]
    StringView Extension() const noexcept;

    /// Appends a `/`-joined segment in place (normalizing any `\` in
    /// `segment` too).
    Result<void> Append(
        StringView segment);

    [[nodiscard]]
    static Result<Path> Join(
        memory::Allocator& allocator,
        StringView base,
        StringView segment);

    [[nodiscard]]
    static Result<Path> Join(
        StringView base,
        StringView segment);

    /// Rewrites the path in place: collapses "." segments and repeated/
    /// mixed separators, and resolves ".." against a preceding real
    /// segment where possible (never escaping above a leading ".." in a
    /// relative path, or above a root). Roots are preserved exactly
    /// (Phase 7.9): a UNC share ("//server/share") and a device path's
    /// volume ("//?/C:", "//?/UNC/server/share") are part of the root, so
    /// ".." can't cross them, and a drive-relative path keeps its drive
    /// ("C:foo/../.." becomes "C:.."). Purely textual — does not touch the
    /// filesystem, so it can't know whether a ".." crosses a symlink, and
    /// it does not flag alternate data streams or DOS device names (that
    /// is the job of Kavach's canonicalizer). A relative path that
    /// normalizes to nothing becomes ".".
    Result<void> Normalize();

    void Swap(
        Path& other) noexcept;

    [[nodiscard]]
    operator StringView() const noexcept;

    [[nodiscard]]
    friend bool operator==(
        const Path& lhs,
        const Path& rhs) noexcept
    {
        return lhs.value_.View() == rhs.value_.View();
    }

    [[nodiscard]]
    friend bool operator!=(
        const Path& lhs,
        const Path& rhs) noexcept
    {
        return !(lhs == rhs);
    }

private:

    [[nodiscard]]
    static bool IsAsciiAlpha(
        char c) noexcept;

    /// True when `text` is nothing but a rooted root: "/", "C:/", a UNC
    /// root ("//server/share", with or without a trailing "/"), a device
    /// root ("//?/C:/", "//./PIPE", "//?/UNC/server/share"), or the bare
    /// "//". A drive-relative "C:" is not a root.
    [[nodiscard]]
    static bool IsRootPath(
        StringView text) noexcept;

    /// Splits the root off a '/'-separated path (Phase 7.9).
    ///
    /// `prefix` receives the root text without its trailing separator:
    /// "" for "/" and for relative paths, "C:" for "C:/" and "C:foo",
    /// "//server/share" for UNC, "//?/C:" or "//?/UNC/server/share" for
    /// device paths. `rooted` is true when ".." can never climb above the
    /// root ("/", "C:/", UNC, device); it is false for relative and
    /// drive-relative ("C:foo") paths. Returns how many characters of
    /// `text` the root consumed, including one separator after `prefix`
    /// if there was one.
    [[nodiscard]]
    static SizeType SplitRoot(
        StringView text,
        StringView* prefix,
        bool* rooted) noexcept;

private:

    String value_;
};

} // namespace forge::core

#include "Path.inl"
