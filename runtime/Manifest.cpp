// forge.json validation and "exports" resolution (Phase 9.2). See Manifest.h.

#include "runtime/Manifest.h"

#include <cstdio>
#include <utility>

namespace forge::runtime
{

using forge::core::Size;
using forge::core::String;
using forge::core::StringView;
using json::Kind;
using json::Node;

namespace
{

constexpr Size kMaxNameLength = 214;
constexpr Size kMaxFieldLength = 4096;
constexpr Size kMaxDependencies = 1000;
constexpr Size kMaxPermissionEntries = 64;  // per list
constexpr Size kMaxEnvNameLength = 256;

bool StartsWith(StringView text, StringView prefix) noexcept
{
    if (text.Size() < prefix.Size())
    {
        return false;
    }

    for (Size i = 0; i < prefix.Size(); ++i)
    {
        if (text.Data()[i] != prefix.Data()[i])
        {
            return false;
        }
    }

    return true;
}

bool EndsWith(StringView text, StringView suffix) noexcept
{
    if (text.Size() < suffix.Size())
    {
        return false;
    }

    return StringView(text.Data() + text.Size() - suffix.Size(), suffix.Size()) == suffix;
}

bool EqualsIgnoreCase(StringView a, const char* b) noexcept
{
    const StringView bv(b);

    if (a.Size() != bv.Size())
    {
        return false;
    }

    for (Size i = 0; i < a.Size(); ++i)
    {
        char c = a.Data()[i];

        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }

        if (c != bv.Data()[i])
        {
            return false;
        }
    }

    return true;
}

Size CountChar(StringView text, char c) noexcept
{
    Size n = 0;

    for (Size i = 0; i < text.Size(); ++i)
    {
        n += text.Data()[i] == c ? 1 : 0;
    }

    return n;
}

Size IndexOf(StringView text, char c) noexcept
{
    for (Size i = 0; i < text.Size(); ++i)
    {
        if (text.Data()[i] == c)
        {
            return i;
        }
    }

    return text.Size();
}

bool IsNameChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
           c == '~';
}

bool IsValidNamePart(StringView part) noexcept
{
    if (part.Empty() || part.Data()[0] == '.' || part.Data()[0] == '_')
    {
        return false;
    }

    for (Size i = 0; i < part.Size(); ++i)
    {
        if (!IsNameChar(part.Data()[i]))
        {
            return false;
        }
    }

    return true;
}

/// A target inside the package: "./" followed by non-empty segments with
/// no ".", "..", "node_modules", '\', '%', ':' or control characters.
/// '*' is allowed only in a pattern target, before substitution.
bool IsSafeTarget(StringView target, bool allowStar) noexcept
{
    if (!StartsWith(target, StringView("./")) || target.Size() == 2 || target.Size() > kMaxFieldLength)
    {
        return false;
    }

    Size start = 2;

    while (start <= target.Size())
    {
        Size end = start;

        while (end < target.Size() && target.Data()[end] != '/')
        {
            const unsigned char c = static_cast<unsigned char>(target.Data()[end]);

            if (c == '\\' || c == '%' || c == ':' || c < 0x20 || c == 0x7F || (c == '*' && !allowStar))
            {
                return false;
            }

            ++end;
        }

        const StringView segment(target.Data() + start, end - start);

        if (segment.Empty() || segment == StringView(".") || segment == StringView("..") ||
            EqualsIgnoreCase(segment, "node_modules"))
        {
            return false;
        }

        if (end >= target.Size())
        {
            break;
        }

        start = end + 1;
    }

    return true;
}

bool AppendMessage(String& message, StringView a, StringView b = StringView(),
                   StringView c = StringView()) noexcept
{
    return message.Append(a).HasValue() && message.Append(b).HasValue() &&
           message.Append(c).HasValue();
}

/// Phase 9.4: one list of "permissions" or of a grant ("read", "write" or
/// "env"). `where` names the list in messages.
bool ParsePermissionList(const json::Document& doc, const Node& node, StringView where,
                         StringView kind, bool allowAbsolute,
                         forge::core::Vector<String>& out, String& message) noexcept
{
    if (node.kind != Kind::Array)
    {
        AppendMessage(message, where, StringView("."), kind);
        AppendMessage(message, StringView(" must be an array of strings"));
        return false;
    }

    if (node.children.Size() > kMaxPermissionEntries)
    {
        AppendMessage(message, where, StringView("."), kind);
        AppendMessage(message, StringView(" lists more than 64 entries"));
        return false;
    }

    const bool isEnv = kind == StringView("env");

    for (Size i = 0; i < node.children.Size(); ++i)
    {
        const Node& item = doc.At(node.children[i]);
        const char* problem = nullptr;

        if (item.kind != Kind::String || item.text.Empty())
        {
            problem = " must hold only non-empty strings";
        }
        else if (item.text.Size() > (isEnv ? kMaxEnvNameLength : kMaxFieldLength))
        {
            problem = isEnv ? " holds a variable name longer than 256 bytes"
                            : " holds a path longer than 4096 bytes";
        }
        else
        {
            const StringView text = item.text.View();

            for (Size c = 0; c < text.Size() && problem == nullptr; ++c)
            {
                const unsigned char ch = static_cast<unsigned char>(text.Data()[c]);

                if (ch < 0x20 || ch == 0x7F || (isEnv && ch == '='))
                {
                    problem = isEnv ? " holds an invalid variable name"
                                    : " holds a path with a control character";
                }
            }

            const bool absolute = text.Data()[0] == '/' || text.Data()[0] == '\\' ||
                                  (text.Size() >= 2 && text.Data()[1] == ':');

            if (problem == nullptr && !isEnv && absolute && !allowAbsolute)
            {
                problem = " must hold paths relative to the program's folder (such as \"./data\"); "
                          "only the program's \"grants\" may name an absolute path";
            }
        }

        if (problem != nullptr)
        {
            AppendMessage(message, where, StringView("."), kind);
            AppendMessage(message, StringView(problem));
            return false;
        }

        String copy;

        if (copy.Append(item.text.View()).HasError() || out.PushBack(std::move(copy)).HasError())
        {
            AppendMessage(message, StringView("out of memory"));
            return false;
        }
    }

    return true;
}

/// Phase 9.4: a permission set: an object with "read", "write" and "env".
bool ParsePermissionSet(const json::Document& doc, const Node& node, StringView where,
                        bool allowAbsolute, PermissionSet& out, String& message) noexcept
{
    if (node.kind != Kind::Object)
    {
        AppendMessage(message, where, StringView(" must be an object"));
        return false;
    }

    for (Size i = 0; i < node.keys.Size(); ++i)
    {
        const StringView kind = node.keys[i].View();
        const Node& list = doc.At(node.children[i]);
        forge::core::Vector<String>* target = kind == StringView("read")    ? &out.read
                                              : kind == StringView("write") ? &out.write
                                              : kind == StringView("env")   ? &out.env
                                                                            : nullptr;

        if (target == nullptr)
        {
            AppendMessage(message, where, StringView(": unknown permission \""), kind);
            AppendMessage(message, StringView("\" (Forge knows read, write and env)"));
            return false;
        }

        if (!ParsePermissionList(doc, list, where, kind, allowAbsolute, *target, message))
        {
            return false;
        }
    }

    return true;
}

bool IsConditionsObject(const Node& node) noexcept
{
    for (Size i = 0; i < node.keys.Size(); ++i)
    {
        if (StartsWith(node.keys[i].View(), StringView(".")))
        {
            return false;
        }
    }

    return true;
}

/// Checks one exports target: a string, null, or a conditions object.
bool ValidateTarget(const json::Document& doc, const Node& node, bool patternKey, String& message) noexcept
{
    switch (node.kind)
    {
    case Kind::Null:
        return true;

    case Kind::String:
        if (!IsSafeTarget(node.text.View(), true))
        {
            AppendMessage(message, StringView("\"exports\": the target \""), node.text.View(),
                                 StringView("\" must start with ./ and stay inside the package (no ., .., "
                                            "node_modules, \\, % or : in it)"));
            return false;
        }

        if (!patternKey && CountChar(node.text.View(), '*') != 0)
        {
            AppendMessage(message, StringView("\"exports\": the target \""), node.text.View(),
                                 StringView("\" has a * but its key has none"));
            return false;
        }

        return true;

    case Kind::Object:
        if (!IsConditionsObject(node))
        {
            AppendMessage(message, StringView("\"exports\": a condition object must not contain "
                                                     "subpaths (keys starting with .)"));
            return false;
        }

        for (Size i = 0; i < node.children.Size(); ++i)
        {
            if (!ValidateTarget(doc, doc.At(node.children[i]), patternKey, message))
            {
                return false;
            }
        }

        return true;

    case Kind::Array:
        AppendMessage(message, StringView("\"exports\": arrays of fallbacks are not supported "
                                          "yet; give one target"));
        return false;

    default:
        AppendMessage(message, StringView("\"exports\": a target must be a string, null or a "
                                                 "condition object"));
        return false;
    }
}

bool ValidateExports(const json::Document& doc, const Node& exports, String& message) noexcept
{
    if (exports.kind == Kind::String || (exports.kind == Kind::Object && IsConditionsObject(exports)))
    {
        if (exports.kind == Kind::String && CountChar(exports.text.View(), '*') != 0)
        {
            AppendMessage(message, StringView("\"exports\": a single target cannot have a *"));
            return false;
        }

        return ValidateTarget(doc, exports, false, message);
    }

    if (exports.kind != Kind::Object)
    {
        AppendMessage(message, StringView("\"exports\" must be a string or an object"));
        return false;
    }

    // A subpath map: every key is "." or starts with "./".
    for (Size i = 0; i < exports.keys.Size(); ++i)
    {
        const StringView key = exports.keys[i].View();

        if (!(key == StringView(".")) && !StartsWith(key, StringView("./")))
        {
            AppendMessage(message, StringView("\"exports\": the key \""), key,
                          StringView("\" mixes subpaths and conditions; put conditions inside a "
                                     "subpath"));
            return false;
        }

        const Size stars = CountChar(key, '*');

        if (stars > 1)
        {
            AppendMessage(message, StringView("\"exports\": the key \""), key,
                                 StringView("\" has more than one *"));
            return false;
        }

        if (!(key == StringView(".")) && !IsSafeTarget(key, true))
        {
            AppendMessage(message, StringView("\"exports\": the key \""), key,
                                 StringView("\" is not a valid subpath"));
            return false;
        }

        if (!ValidateTarget(doc, doc.At(exports.children[i]), stars == 1, message))
        {
            return false;
        }
    }

    return true;
}

bool IsSupportedCondition(StringView key) noexcept
{
    return key == StringView("forge") || key == StringView("import") || key == StringView("default");
}

/// Resolves one target. `star` is the text a pattern matched, or null.
ExportResult ResolveTarget(const json::Document& doc, const Node& node, const StringView* star,
                           String& target) noexcept
{
    switch (node.kind)
    {
    case Kind::Null:
        return ExportResult::Blocked;

    case Kind::String:
    {
        target.Clear();
        const StringView text = node.text.View();

        for (Size i = 0; i < text.Size(); ++i)
        {
            const bool ok = (text.Data()[i] == '*' && star != nullptr)
                                ? target.Append(*star).HasValue()
                                : target.Append(text.Data()[i]).HasValue();

            if (!ok)
            {
                return ExportResult::Invalid;
            }
        }

        return IsSafeTarget(target.View(), false) ? ExportResult::Found : ExportResult::Invalid;
    }

    case Kind::Object:
        for (Size i = 0; i < node.keys.Size(); ++i)
        {
            if (!IsSupportedCondition(node.keys[i].View()))
            {
                continue;
            }

            const ExportResult result = ResolveTarget(doc, doc.At(node.children[i]), star, target);

            if (result != ExportResult::NotExported)
            {
                return result;
            }
        }

        return ExportResult::NotExported;

    default:
        return ExportResult::NotExported;
    }
}

} // namespace

bool IsValidPackageName(StringView name) noexcept
{
    if (name.Empty() || name.Size() > kMaxNameLength)
    {
        return false;
    }

    if (name.Data()[0] == '@')
    {
        const Size slash = IndexOf(name, '/');

        if (slash >= name.Size())
        {
            return false;
        }

        return IsValidNamePart(StringView(name.Data() + 1, slash - 1)) &&
               IsValidNamePart(StringView(name.Data() + slash + 1, name.Size() - slash - 1));
    }

    return IsValidNamePart(name);
}

bool SplitBareSpecifier(StringView specifier, String& name, String& subpath) noexcept
{
    name.Clear();
    subpath.Clear();

    Size end = IndexOf(specifier, '/');

    if (!specifier.Empty() && specifier.Data()[0] == '@' && end < specifier.Size())
    {
        const StringView rest(specifier.Data() + end + 1, specifier.Size() - end - 1);
        end = end + 1 + IndexOf(rest, '/');
    }

    const StringView pkg(specifier.Data(), end);

    if (!IsValidPackageName(pkg) || name.Append(pkg).HasError())
    {
        return false;
    }

    if (end >= specifier.Size())
    {
        return subpath.Append('.').HasValue();
    }

    // "pkg/sub/x.js" -> "./sub/x.js"; it must be a safe subpath.
    if (subpath.Append('.').HasError() ||
        subpath.Append(StringView(specifier.Data() + end, specifier.Size() - end)).HasError())
    {
        return false;
    }

    return IsSafeTarget(subpath.View(), false);
}

bool ParseManifest(StringView bytes, bool isApp, Manifest& out, String& message) noexcept
{
    message.Clear();
    out = Manifest();

    json::ParseError error;

    if (!json::Parse(bytes, out.document, error))
    {
        char where[40];
        snprintf(where, sizeof(where), " (at byte %llu)",
                 static_cast<unsigned long long>(error.offset));
        AppendMessage(message, StringView("not valid JSON: "), StringView(error.message),
                      StringView(where));
        return false;
    }

    const json::Document& doc = out.document;
    const Node& root = doc.Root();

    if (root.kind != Kind::Object)
    {
        AppendMessage(message, StringView("the top level must be an object"));
        return false;
    }

    static const char* const kKnown[] = { "name", "version", "description", "license", "author",
                                          "exports", "dependencies", "permissions", "grants" };

    for (Size i = 0; i < root.keys.Size(); ++i)
    {
        const StringView key = root.keys[i].View();
        bool known = false;

        for (const char* k : kKnown)
        {
            known = known || key == StringView(k);
        }

        if (known)
        {
            continue;
        }

        if (key == StringView("main") || key == StringView("module") || key == StringView("type"))
        {
            AppendMessage(message, StringView("the field \""), key,
                          StringView("\" is not used by Forge; every file is an ES module and entry "
                                     "points are listed in \"exports\""));
        }
        else if (key == StringView("scripts"))
        {
            AppendMessage(message, StringView("the field \"scripts\" is refused: Forge never runs "
                                              "package scripts"));
        }
        else
        {
            AppendMessage(message, StringView("unknown field \""), key,
                          StringView("\" (Forge reads name, version, description, license, author, "
                                     "exports, dependencies, permissions and grants)"));
        }

        return false;
    }

    for (Size i = 0; i < root.keys.Size(); ++i)
    {
        const StringView key = root.keys[i].View();
        const Node& value = doc.At(root.children[i]);

        if (key == StringView("name"))
        {
            if (value.kind != Kind::String || !IsValidPackageName(value.text.View()))
            {
                AppendMessage(message, StringView("\"name\" must be a valid package name (lower-case "
                                                  "letters, digits, - . _ ~, optionally @scope/)"));
                return false;
            }

            if (out.name.Append(value.text.View()).HasError())
            {
                AppendMessage(message, StringView("out of memory"));
                return false;
            }
        }
        else if (key == StringView("version") || key == StringView("description") ||
                 key == StringView("license") || key == StringView("author"))
        {
            if (value.kind != Kind::String || value.text.Size() > kMaxFieldLength)
            {
                AppendMessage(message, StringView("\""), key,
                              StringView("\" must be a string of at most 4096 bytes"));
                return false;
            }

            if (key == StringView("version") && out.version.Append(value.text.View()).HasError())
            {
                AppendMessage(message, StringView("out of memory"));
                return false;
            }
        }
        else if (key == StringView("permissions"))
        {
            // Phase 9.4: what this package asks for, relative to the program.
            if (!ParsePermissionSet(doc, value, StringView("\"permissions\""), false,
                                    out.permissions, message))
            {
                return false;
            }
        }
        else if (key == StringView("grants"))
        {
            // Phase 9.4: what the program grants each package, by name.
            if (value.kind != Kind::Object)
            {
                AppendMessage(message, StringView("\"grants\" must be an object"));
                return false;
            }

            if (value.keys.Size() > kMaxDependencies)
            {
                AppendMessage(message, StringView("\"grants\" names more than 1000 packages"));
                return false;
            }

            for (Size g = 0; g < value.keys.Size(); ++g)
            {
                const StringView grantee = value.keys[g].View();

                if (!IsValidPackageName(grantee))
                {
                    AppendMessage(message, StringView("\"grants\": \""), grantee,
                                  StringView("\" is not a valid package name"));
                    return false;
                }

                String where;
                PackageGrant grant;

                if (where.Append(StringView("\"grants\".\"")).HasError() ||
                    where.Append(grantee).HasError() || where.Append('"').HasError() ||
                    grant.name.Append(grantee).HasError())
                {
                    AppendMessage(message, StringView("out of memory"));
                    return false;
                }

                if (!ParsePermissionSet(doc, doc.At(value.children[g]), where.View(), true,
                                        grant.permissions, message))
                {
                    return false;
                }

                if (out.grants.PushBack(std::move(grant)).HasError())
                {
                    AppendMessage(message, StringView("out of memory"));
                    return false;
                }
            }
        }
        else if (key == StringView("dependencies"))
        {
            if (value.kind != Kind::Object)
            {
                AppendMessage(message, StringView("\"dependencies\" must be an object"));
                return false;
            }

            if (value.keys.Size() > kMaxDependencies)
            {
                AppendMessage(message, StringView("\"dependencies\" lists more than 1000 packages"));
                return false;
            }

            for (Size d = 0; d < value.keys.Size(); ++d)
            {
                const StringView depName = value.keys[d].View();
                const Node& spec = doc.At(value.children[d]);

                if (!IsValidPackageName(depName))
                {
                    AppendMessage(message, StringView("\"dependencies\": \""), depName,
                                  StringView("\" is not a valid package name"));
                    return false;
                }

                if (spec.kind != Kind::String || !StartsWith(spec.text.View(), StringView("file:")) ||
                    spec.text.Size() == 5 || spec.text.Size() > kMaxFieldLength)
                {
                    AppendMessage(message, StringView("\"dependencies\": \""), depName,
                                  StringView("\" must be \"file:<folder>\"; versions and registries come "
                                             "with the package manager (Phase 14)"));
                    return false;
                }

                Dependency dependency;

                if (dependency.name.Append(depName).HasError() ||
                    dependency.path.Append(StringView(spec.text.Data() + 5, spec.text.Size() - 5))
                        .HasError() ||
                    out.dependencies.PushBack(std::move(dependency)).HasError())
                {
                    AppendMessage(message, StringView("out of memory"));
                    return false;
                }
            }
        }
        else if (key == StringView("exports"))
        {
            if (!ValidateExports(doc, value, message))
            {
                return false;
            }

            out.hasExports = true;
            out.exportsNode = root.children[i];
        }
    }

    if (!isApp && out.name.Empty())
    {
        AppendMessage(message, StringView("\"name\" is required for a package that others depend on"));
        return false;
    }

    return true;
}

ExportResult ResolveExport(const Manifest& manifest, StringView subpath, String& target) noexcept
{
    target.Clear();

    if (!manifest.hasExports)
    {
        return ExportResult::NoExports;
    }

    const json::Document& doc = manifest.document;
    const Node& exports = doc.At(manifest.exportsNode);

    if (exports.kind == Kind::String || IsConditionsObject(exports))
    {
        if (!(subpath == StringView(".")))
        {
            return ExportResult::NotExported;
        }

        return ResolveTarget(doc, exports, nullptr, target);
    }

    // Exact keys first.
    for (Size i = 0; i < exports.keys.Size(); ++i)
    {
        const StringView key = exports.keys[i].View();

        if (CountChar(key, '*') == 0 && key == subpath)
        {
            return ResolveTarget(doc, doc.At(exports.children[i]), nullptr, target);
        }
    }

    // Then the best pattern: the longest text before the *, then the
    // longest key (Node.js's PATTERN_KEY_COMPARE).
    Size best = exports.keys.Size();
    Size bestPrefix = 0;
    Size bestLength = 0;

    for (Size i = 0; i < exports.keys.Size(); ++i)
    {
        const StringView key = exports.keys[i].View();
        const Size star = IndexOf(key, '*');

        if (star >= key.Size())
        {
            continue;
        }

        const StringView prefix(key.Data(), star);
        const StringView suffix(key.Data() + star + 1, key.Size() - star - 1);

        if (subpath.Size() < key.Size() || !StartsWith(subpath, prefix) || !EndsWith(subpath, suffix))
        {
            continue;
        }

        if (best == exports.keys.Size() || prefix.Size() > bestPrefix ||
            (prefix.Size() == bestPrefix && key.Size() > bestLength))
        {
            best = i;
            bestPrefix = prefix.Size();
            bestLength = key.Size();
        }
    }

    if (best == exports.keys.Size())
    {
        return ExportResult::NotExported;
    }

    const StringView key = exports.keys[best].View();
    const Size star = IndexOf(key, '*');
    const Size suffixLength = key.Size() - star - 1;
    const StringView matched(subpath.Data() + star, subpath.Size() - star - suffixLength);
    return ResolveTarget(doc, doc.At(exports.children[best]), &matched, target);
}

} // namespace forge::runtime
