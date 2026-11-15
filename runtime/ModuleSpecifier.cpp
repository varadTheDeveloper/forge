// Module specifier resolution (Phase 9.1). See ModuleSpecifier.h.

#include "runtime/ModuleSpecifier.h"

#include <utility>

#include "forge-core/memory/Vector.h"

namespace forge::runtime
{

using forge::core::Size;
using forge::core::String;
using forge::core::StringView;
using forge::core::Vector;

namespace
{

constexpr Size kMaxLength = 32767;

bool IsAsciiLetter(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsAsciiDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

int HexValue(char c) noexcept
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }

    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }

    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }

    return -1;
}

bool StartsWith(StringView text, const char* prefix) noexcept
{
    const StringView p(prefix);

    if (text.Size() < p.Size())
    {
        return false;
    }

    for (Size i = 0; i < p.Size(); ++i)
    {
        if (text.Data()[i] != p.Data()[i])
        {
            return false;
        }
    }

    return true;
}

char ToLowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Bytes left as they are in a normalized URL segment.
bool IsUnencoded(unsigned char c) noexcept
{
    if (IsAsciiLetter(static_cast<char>(c)) || IsAsciiDigit(static_cast<char>(c)))
    {
        return true;
    }

    switch (c)
    {
    case '-': case '.': case '_': case '~': case '!': case '$': case '&': case '\'':
    case '(': case ')': case '*': case '+': case ',': case ';': case '=': case ':':
    case '@':
        return true;
    default:
        return false;
    }
}

/// Percent-decodes one segment into raw bytes, refusing malformed escapes,
/// escaped separators and control characters.
SpecifierError DecodeSegment(StringView segment, String& out) noexcept
{
    out.Clear();

    for (Size i = 0; i < segment.Size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(segment.Data()[i]);

        if (c == '%')
        {
            if (i + 2 >= segment.Size())
            {
                return SpecifierError::BadEscape;
            }

            const int hi = HexValue(segment.Data()[i + 1]);
            const int lo = HexValue(segment.Data()[i + 2]);

            if (hi < 0 || lo < 0)
            {
                return SpecifierError::BadEscape;
            }

            c = static_cast<unsigned char>(hi * 16 + lo);
            i += 2;

            if (c == '/' || c == '\\')
            {
                return SpecifierError::EncodedSeparator;
            }
        }

        if (c < 0x20 || c == 0x7F)
        {
            return SpecifierError::InvalidCharacter;
        }

        if (out.Append(static_cast<char>(c)).HasError())
        {
            return SpecifierError::OutOfMemory;
        }
    }

    return SpecifierError::None;
}

/// Appends raw bytes as a normalized URL segment.
bool EncodeSegment(StringView raw, String& out) noexcept
{
    static const char kHex[] = "0123456789ABCDEF";

    for (Size i = 0; i < raw.Size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(raw.Data()[i]);

        if (IsUnencoded(c))
        {
            if (out.Append(static_cast<char>(c)).HasError())
            {
                return false;
            }
        }
        else if (out.Append('%').HasError() || out.Append(kHex[c >> 4]).HasError() ||
                 out.Append(kHex[c & 15]).HasError())
        {
            return false;
        }
    }

    return true;
}

/// Splits `path` at '/' (and '\', which the URL standard treats as '/' in
/// file: URLs) and applies each segment to `segments`, decoded. The first
/// entry of `segments` is the drive ("C:") and is never removed.
SpecifierError ApplySegments(StringView path, Vector<String>& segments) noexcept
{
    Size start = 0;

    while (start <= path.Size())
    {
        Size end = start;

        while (end < path.Size() && path.Data()[end] != '/' && path.Data()[end] != '\\')
        {
            ++end;
        }

        const StringView part(path.Data() + start, end - start);
        String decoded;
        const SpecifierError error = DecodeSegment(part, decoded);

        if (error != SpecifierError::None)
        {
            return error;
        }

        const StringView d = decoded.View();
        const bool isDot = d.Size() == 1 && d.Data()[0] == '.';
        const bool isDotDot = d.Size() == 2 && d.Data()[0] == '.' && d.Data()[1] == '.';

        if (isDotDot)
        {
            if (segments.Size() > 1)
            {
                segments.PopBack();
            }
        }
        else if (!isDot && !d.Empty())
        {
            if (segments.PushBack(std::move(decoded)).HasError())
            {
                return SpecifierError::OutOfMemory;
            }
        }

        if (end >= path.Size())
        {
            break;
        }

        start = end + 1;
    }

    return SpecifierError::None;
}

SpecifierError BuildUrl(const Vector<String>& segments, String& outUrl) noexcept
{
    outUrl.Clear();

    if (outUrl.Append(StringView("file:///")).HasError() ||
        outUrl.Append(segments[0].View()).HasError())
    {
        return SpecifierError::OutOfMemory;
    }

    for (Size i = 1; i < segments.Size(); ++i)
    {
        if (outUrl.Append('/').HasError() || !EncodeSegment(segments[i].View(), outUrl))
        {
            return SpecifierError::OutOfMemory;
        }
    }

    if (segments.Size() == 1 && outUrl.Append('/').HasError())
    {
        return SpecifierError::OutOfMemory;
    }

    return outUrl.Size() > kMaxLength ? SpecifierError::TooLong : SpecifierError::None;
}

/// "C:" or "c|" (the legacy form) as the first segment of a file: URL path.
bool IsDriveSegment(StringView segment) noexcept
{
    return segment.Size() == 2 && IsAsciiLetter(segment.Data()[0]) &&
           (segment.Data()[1] == ':' || segment.Data()[1] == '|');
}

/// Parses the path of a file: URL ("/C:/a/b") into segments.
SpecifierError ParseFileUrlPath(StringView path, Vector<String>& segments) noexcept
{
    // Skip the leading separators, then expect the drive.
    Size i = 0;

    while (i < path.Size() && (path.Data()[i] == '/' || path.Data()[i] == '\\'))
    {
        ++i;
    }

    if (path.Size() - i < 2 || !IsDriveSegment(StringView(path.Data() + i, 2)) ||
        (path.Size() - i > 2 && path.Data()[i + 2] != '/' && path.Data()[i + 2] != '\\'))
    {
        return SpecifierError::BadFileUrl;
    }

    String drive;
    char letter = path.Data()[i];

    if (letter >= 'a' && letter <= 'z')
    {
        letter = static_cast<char>(letter - 'a' + 'A');
    }

    if (drive.Append(letter).HasError() || drive.Append(':').HasError() ||
        segments.PushBack(std::move(drive)).HasError())
    {
        return SpecifierError::OutOfMemory;
    }

    return ApplySegments(StringView(path.Data() + i + 2, path.Size() - i - 2), segments);
}

/// The scheme of `text` ("https" for "https://x"), or an empty view when it
/// does not start with one.
StringView SchemeOf(StringView text) noexcept
{
    if (text.Empty() || !IsAsciiLetter(text.Data()[0]))
    {
        return StringView();
    }

    for (Size i = 1; i < text.Size(); ++i)
    {
        const char c = text.Data()[i];

        if (c == ':')
        {
            return StringView(text.Data(), i);
        }

        if (!IsAsciiLetter(c) && !IsAsciiDigit(c) && c != '+' && c != '-' && c != '.')
        {
            return StringView();
        }
    }

    return StringView();
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
        if (ToLowerAscii(a.Data()[i]) != bv.Data()[i])
        {
            return false;
        }
    }

    return true;
}

bool HasQueryOrFragment(StringView text) noexcept
{
    for (Size i = 0; i < text.Size(); ++i)
    {
        if (text.Data()[i] == '?' || text.Data()[i] == '#')
        {
            return true;
        }
    }

    return false;
}

} // namespace

const char* DescribeSpecifierError(SpecifierError error) noexcept
{
    switch (error)
    {
    case SpecifierError::None: return "no error";
    case SpecifierError::Empty: return "the specifier is empty";
    case SpecifierError::Bare:
        return "a package name is resolved through the \"dependencies\" in forge.json";
    case SpecifierError::NodeBuiltin: return "Forge has no Node built-in modules";
    case SpecifierError::UnsupportedScheme:
        return "Forge loads modules only from files (never from the network, data: or "
               "package-manager URLs)";
    case SpecifierError::WindowsPath:
        return "a Windows path is not a module specifier; write a relative path such as "
               "./lib.js or a file:///C:/... URL";
    case SpecifierError::QueryOrFragment:
        return "a query (?) or fragment (#) would give one file two module identities";
    case SpecifierError::BadEscape: return "a % must be followed by two hexadecimal digits";
    case SpecifierError::EncodedSeparator: return "an escaped / or \\ (%2F, %5C) is not allowed";
    case SpecifierError::InvalidCharacter: return "it contains a NUL or control character";
    case SpecifierError::NetworkFileUrl: return "modules are never loaded from a network share";
    case SpecifierError::BadFileUrl: return "a file: URL must name a drive (file:///C:/...)";
    case SpecifierError::NoBase: return "a relative specifier needs an importing module";
    case SpecifierError::TooLong: return "it is too long";
    case SpecifierError::OutOfMemory: return "out of memory";
    }

    return "unknown error";
}

const char* SpecifierErrorCode(SpecifierError error) noexcept
{
    switch (error)
    {
    case SpecifierError::Bare:
    case SpecifierError::NodeBuiltin:
    case SpecifierError::UnsupportedScheme:
    case SpecifierError::NetworkFileUrl:
        return "NotSupported";
    case SpecifierError::OutOfMemory:
        return "OutOfMemory";
    default:
        return "InvalidArgument";
    }
}

SpecifierError ResolveSpecifier(StringView specifier, StringView baseUrl, String& outUrl) noexcept
{
    outUrl.Clear();

    if (specifier.Empty())
    {
        return SpecifierError::Empty;
    }

    if (specifier.Size() > kMaxLength)
    {
        return SpecifierError::TooLong;
    }

    const StringView scheme = SchemeOf(specifier);
    Vector<String> segments;

    if (!scheme.Empty())
    {
        if (scheme.Size() == 1)
        {
            return SpecifierError::WindowsPath;  // "C:/x.js", "C:\x.js", "C:x.js"
        }

        if (EqualsIgnoreCase(scheme, "node"))
        {
            return SpecifierError::NodeBuiltin;
        }

        if (!EqualsIgnoreCase(scheme, "file"))
        {
            return SpecifierError::UnsupportedScheme;
        }

        if (HasQueryOrFragment(specifier))
        {
            return SpecifierError::QueryOrFragment;
        }

        // file: URL. "file://" + host + path, or "file:/path".
        StringView rest(specifier.Data() + 5, specifier.Size() - 5);

        if (StartsWith(rest, "//") || StartsWith(rest, "\\\\"))
        {
            rest = StringView(rest.Data() + 2, rest.Size() - 2);
            Size hostEnd = 0;

            while (hostEnd < rest.Size() && rest.Data()[hostEnd] != '/' &&
                   rest.Data()[hostEnd] != '\\')
            {
                ++hostEnd;
            }

            const StringView host(rest.Data(), hostEnd);

            if (!host.Empty() && !EqualsIgnoreCase(host, "localhost"))
            {
                // "file://C:/x" is the drive written as a host; browsers
                // accept it, and so does Forge. Anything else is a server.
                if (!IsDriveSegment(host))
                {
                    return SpecifierError::NetworkFileUrl;
                }

                hostEnd = 0;
            }

            rest = StringView(rest.Data() + hostEnd, rest.Size() - hostEnd);
        }

        const SpecifierError error = ParseFileUrlPath(rest, segments);

        if (error != SpecifierError::None)
        {
            return error;
        }

        return BuildUrl(segments, outUrl);
    }

    const bool rootRelative = StartsWith(specifier, "/") || StartsWith(specifier, "\\");
    const bool relative = rootRelative || StartsWith(specifier, "./") ||
                          StartsWith(specifier, "../") || StartsWith(specifier, ".\\") ||
                          StartsWith(specifier, "..\\");

    if (!relative)
    {
        return SpecifierError::Bare;
    }

    if (HasQueryOrFragment(specifier))
    {
        return SpecifierError::QueryOrFragment;
    }

    if (baseUrl.Empty() || !StartsWith(baseUrl, "file:///"))
    {
        return SpecifierError::NoBase;
    }

    // The base is a normalized URL: "file:///C:/a/b.js".
    SpecifierError error =
        ParseFileUrlPath(StringView(baseUrl.Data() + 7, baseUrl.Size() - 7), segments);

    if (error != SpecifierError::None)
    {
        return error;
    }

    if (rootRelative)
    {
        while (segments.Size() > 1)
        {
            segments.PopBack();
        }
    }
    else if (segments.Size() > 1)
    {
        segments.PopBack();  // the importing module's own file name
    }

    error = ApplySegments(specifier, segments);

    if (error != SpecifierError::None)
    {
        return error;
    }

    return BuildUrl(segments, outUrl);
}

SpecifierError FileUrlToPath(StringView url, String& outPath) noexcept
{
    outPath.Clear();

    if (!StartsWith(url, "file:///"))
    {
        return SpecifierError::BadFileUrl;
    }

    Vector<String> segments;
    const SpecifierError error =
        ParseFileUrlPath(StringView(url.Data() + 7, url.Size() - 7), segments);

    if (error != SpecifierError::None)
    {
        return error;
    }

    if (outPath.Append(segments[0].View()).HasError())
    {
        return SpecifierError::OutOfMemory;
    }

    for (Size i = 1; i < segments.Size(); ++i)
    {
        if (outPath.Append('\\').HasError() || outPath.Append(segments[i].View()).HasError())
        {
            return SpecifierError::OutOfMemory;
        }
    }

    if (segments.Size() == 1 && outPath.Append('\\').HasError())
    {
        return SpecifierError::OutOfMemory;
    }

    return SpecifierError::None;
}

bool PathToFileUrl(StringView path, String& outUrl) noexcept
{
    outUrl.Clear();

    if (path.Size() < 2 || !IsAsciiLetter(path.Data()[0]) || path.Data()[1] != ':' ||
        (path.Size() > 2 && path.Data()[2] != '\\' && path.Data()[2] != '/'))
    {
        return false;
    }

    Vector<String> segments;
    String drive;
    char letter = path.Data()[0];

    if (letter >= 'a' && letter <= 'z')
    {
        letter = static_cast<char>(letter - 'a' + 'A');
    }

    if (drive.Append(letter).HasError() || drive.Append(':').HasError() ||
        segments.PushBack(std::move(drive)).HasError())
    {
        return false;
    }

    // Raw names, not escapes: a '%' in a Windows file name is encoded.
    Size start = 2;

    while (start < path.Size())
    {
        while (start < path.Size() && (path.Data()[start] == '\\' || path.Data()[start] == '/'))
        {
            ++start;
        }

        Size end = start;

        while (end < path.Size() && path.Data()[end] != '\\' && path.Data()[end] != '/')
        {
            ++end;
        }

        if (end > start)
        {
            String name;

            if (name.Append(StringView(path.Data() + start, end - start)).HasError() ||
                segments.PushBack(std::move(name)).HasError())
            {
                return false;
            }
        }

        start = end;
    }

    return BuildUrl(segments, outUrl) == SpecifierError::None;
}

} // namespace forge::runtime
