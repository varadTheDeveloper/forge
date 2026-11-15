#pragma once

// Module specifiers (Phase 9.1, PHASE9_DESIGN.md section 3).
//
// Pure string code with Windows path semantics and no JSAPI, so it also
// builds in the Linux sanitizer job (runtime/ModuleSpecifierTest.cpp).
//
// A module's identity is a normalized file URL:
//   file:///C:/dir/sub/file.js
// with an upper-case drive letter, no "." or ".." segments, no empty
// segments, no query or fragment, and every byte outside
//   A-Z a-z 0-9 - . _ ~ ! $ & ' ( ) * + , ; = : @
// percent-encoded with upper-case hex digits. ResolveSpecifier only ever
// produces this form; FileUrlToPath only accepts it.

#include "forge-core/String.h"
#include "forge-core/StringView.h"

namespace forge::runtime
{

enum class SpecifierError
{
    None,
    Empty,
    Bare,               // "lib", "@scope/lib": a package (resolved by runtime/Modules)
    NodeBuiltin,        // "node:fs": Forge has no Node built-ins
    UnsupportedScheme,  // https:, data:, npm:, jsr:, blob:, ...
    WindowsPath,        // "C:/x.js" or "C:\x.js" written as a specifier
    QueryOrFragment,    // "./a.js?v=1", "./a.js#x"
    BadEscape,          // "%" not followed by two hex digits
    EncodedSeparator,   // "%2F" or "%5C" inside a name
    InvalidCharacter,   // NUL or another control character (raw or escaped)
    NetworkFileUrl,     // file://server/share/x.js
    BadFileUrl,         // a file: URL without a drive letter, or otherwise malformed
    NoBase,             // a relative specifier with nothing to resolve it against
    TooLong,
    OutOfMemory,
};

/// A plain-language reason, for error messages.
const char* DescribeSpecifierError(
    SpecifierError error) noexcept;

/// The code a JavaScript error for `error` carries: "NotSupported" for
/// specifiers Forge does not load by design, otherwise "InvalidArgument".
const char* SpecifierErrorCode(
    SpecifierError error) noexcept;

/// Resolves `specifier` against `baseUrl` (a normalized file URL, or empty
/// when there is no importing module) and writes the normalized file URL
/// of the module it names to `outUrl`.
[[nodiscard]]
SpecifierError ResolveSpecifier(
    forge::core::StringView specifier,
    forge::core::StringView baseUrl,
    forge::core::String& outUrl) noexcept;

/// Converts a normalized file URL to a Windows path ("C:\dir\file.js"),
/// decoding percent escapes.
[[nodiscard]]
SpecifierError FileUrlToPath(
    forge::core::StringView url,
    forge::core::String& outPath) noexcept;

/// Converts a canonical Windows path with a drive letter ("C:\dir\file.js")
/// to a normalized file URL. False for anything else (a network share, a
/// relative path) or when out of memory.
[[nodiscard]]
bool PathToFileUrl(
    forge::core::StringView path,
    forge::core::String& outUrl) noexcept;

} // namespace forge::runtime
