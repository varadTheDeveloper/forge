#pragma once

// forge.json, the package manifest (Phase 9.2, PHASE9_DESIGN.md section 4).
// Portable string code: parsed by runtime/Json, validated here, never
// evaluated as JavaScript. Unit-tested (runtime/ManifestTest.cpp) and fuzzed.
//
//   {
//     "name": "greet",                       // required for a dependency
//     "version": "1.0.0",
//     "exports": {                           // what other packages may import
//       ".": "./index.js",
//       "./extra": { "forge": "./extra-forge.js", "default": "./extra.js" },
//       "./features/*": "./features/*.js",
//       "./internal": null
//     },
//     "dependencies": { "@acme/util": "file:../util" },
//     "permissions": {                       // Phase 9.4: what the package asks for
//       "read": ["./data"], "write": [], "env": ["HOME"]
//     },
//     "grants": {                            // Phase 9.4: the program's grants
//       "greet": { "read": ["./data"] }
//     }
//   }
//
// Paths in "permissions" and "grants" are relative to the program's folder
// (the only place a person decides what a program may touch); only a grant
// may be absolute. Only the program's "grants" count, and "permissions"
// matter only for a dependency; both are validated wherever they appear,
// so the same forge.json works as a program and as a package.
//
// Any other top-level field is refused, so a misspelt field ("dependancies")
// or one Forge never honours ("scripts") is never silently ignored.

#include "forge-core/String.h"
#include "forge-core/StringView.h"
#include "forge-core/Types.h"
#include "forge-core/memory/Vector.h"
#include "runtime/Json.h"

namespace forge::runtime
{

/// The manifest's file name. One constant, so the naming decision (D1)
/// stays a one-line change.
constexpr const char* kManifestName = "forge.json";

/// "file:<path>" from "dependencies". The path is kept as written; the
/// loader resolves it against the declaring package's folder.
struct Dependency
{
    forge::core::String name;
    forge::core::String path;
};

/// Phase 9.4: what a package asks for ("permissions") or what the program
/// grants it (one entry of "grants"). Kept as written; Kavach resolves the
/// paths against the program's folder and checks the variable names.
struct PermissionSet
{
    forge::core::Vector<forge::core::String> read;
    forge::core::Vector<forge::core::String> write;
    forge::core::Vector<forge::core::String> env;

    bool Empty() const noexcept { return read.Empty() && write.Empty() && env.Empty(); }
};

struct PackageGrant
{
    forge::core::String name;
    PermissionSet permissions;
};

struct Manifest
{
    forge::core::String name;  // empty only for an app without a name
    forge::core::String version;
    forge::core::Vector<Dependency> dependencies;
    bool hasExports = false;

    PermissionSet permissions;                     // Phase 9.4
    forge::core::Vector<PackageGrant> grants;      // Phase 9.4

    /// The parsed file, kept for resolving "exports".
    json::Document document;
    forge::core::Size exportsNode = 0;
};

/// Parses and validates forge.json. `isApp` is true for the program's own
/// manifest, where "name" may be left out. On failure `message` says what
/// is wrong, in plain language, without quoting more of the file than a
/// field name.
[[nodiscard]]
bool ParseManifest(
    forge::core::StringView bytes,
    bool isApp,
    Manifest& out,
    forge::core::String& message) noexcept;

/// True for a valid package name: lower-case letters, digits, '-', '.', '_'
/// and '~', not starting with '.' or '_', optionally under one "@scope/",
/// at most 214 characters.
[[nodiscard]]
bool IsValidPackageName(
    forge::core::StringView name) noexcept;

/// Splits a bare specifier ("@acme/util/sub/x.js") into the package name
/// ("@acme/util") and the subpath ("./sub/x.js", or "." for the package
/// itself). False if the name is not valid or the subpath is malformed.
[[nodiscard]]
bool SplitBareSpecifier(
    forge::core::StringView specifier,
    forge::core::String& name,
    forge::core::String& subpath) noexcept;

enum class ExportResult
{
    Found,        // `target` holds a "./..." path inside the package
    NotExported,  // no "exports" entry matches the subpath
    Blocked,      // an entry matches and is null: explicitly private
    NoExports,    // the manifest has no "exports" at all
    Invalid,      // a pattern substitution produced an unsafe target
};

/// Resolves `subpath` ("." or "./x") through the manifest's "exports", with
/// the conditions Forge supports ("forge", "import", "default"), chosen in
/// the order the object lists them, as Node.js does.
[[nodiscard]]
ExportResult ResolveExport(
    const Manifest& manifest,
    forge::core::StringView subpath,
    forge::core::String& target) noexcept;

} // namespace forge::runtime
