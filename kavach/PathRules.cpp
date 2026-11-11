#include "kavach/PathRules.h"

#include "forge-core/memory/Vector.h"

namespace forge::kavach
{

namespace
{

using forge::core::Size;
using forge::core::String;
using forge::core::StringView;

/// Windows' own limit is 32,767 UTF-16 units; UTF-8 needs at most 3 bytes
/// per unit, and a canonical path this long is refused outright.
constexpr Size kMaxPathBytes = 32767;

char FoldAscii(char c) noexcept
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool EqualsIgnoreAsciiCase(StringView a, StringView b) noexcept
{
    if (a.Size() != b.Size())
    {
        return false;
    }

    for (Size i = 0; i < a.Size(); ++i)
    {
        if (FoldAscii(a.Data()[i]) != FoldAscii(b.Data()[i]))
        {
            return false;
        }
    }

    return true;
}

bool IsAsciiLetter(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/// Device names Windows reserves in every directory, with or without an
/// extension ("NUL.txt" is still the NUL device). The digit may also be a
/// superscript 1, 2 or 3 (U+00B9, U+00B2, U+00B3).
bool IsReservedName(StringView component) noexcept
{
    // The device name is the part before the first '.', without trailing
    // spaces ("CON .txt" is CON too).
    Size end = 0;

    while (end < component.Size() && component.Data()[end] != '.')
    {
        ++end;
    }

    while (end > 0 && component.Data()[end - 1] == ' ')
    {
        --end;
    }

    const StringView base(component.Data(), end);

    static const char* const kNames[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };

    for (const char* name : kNames)
    {
        if (EqualsIgnoreAsciiCase(base, StringView(name)))
        {
            return true;
        }
    }

    if (base.Size() < 4)
    {
        return false;
    }

    const StringView prefix(base.Data(), 3);

    if (!EqualsIgnoreAsciiCase(prefix, StringView("COM")) &&
        !EqualsIgnoreAsciiCase(prefix, StringView("LPT")))
    {
        return false;
    }

    const StringView suffix(base.Data() + 3, base.Size() - 3);

    if (suffix.Size() == 1 && IsDigit(suffix.Data()[0]))
    {
        return true;
    }

    // Superscript digits, UTF-8: C2 B9, C2 B2, C2 B3.
    if (suffix.Size() == 2 && static_cast<unsigned char>(suffix.Data()[0]) == 0xC2)
    {
        const unsigned char second = static_cast<unsigned char>(suffix.Data()[1]);
        return second == 0xB9 || second == 0xB2 || second == 0xB3;
    }

    return false;
}

/// "PROGRA~1", "MYDOCU~12.TXT": at most 6 characters, '~', digits, and an
/// optional extension of at most 3 characters.
bool LooksLikeShortName(StringView component) noexcept
{
    Size dot = component.Size();

    for (Size i = 0; i < component.Size(); ++i)
    {
        if (component.Data()[i] == '.')
        {
            dot = i;
            break;
        }
    }

    if (component.Size() - dot > 4)  // ".ext" with more than 3 characters
    {
        return false;
    }

    const StringView base(component.Data(), dot);
    Size tilde = base.Size();

    for (Size i = 0; i < base.Size(); ++i)
    {
        if (base.Data()[i] == '~')
        {
            tilde = i;
            break;
        }
    }

    if (tilde == 0 || tilde > 6 || tilde + 1 >= base.Size())
    {
        return false;
    }

    for (Size i = tilde + 1; i < base.Size(); ++i)
    {
        if (!IsDigit(base.Data()[i]))
        {
            return false;
        }
    }

    return true;
}

PathError CheckComponent(StringView component) noexcept
{
    for (Size i = 0; i < component.Size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(component.Data()[i]);

        if (c == ':')
        {
            return PathError::AlternateStream;
        }

        if (c < 0x20 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
        {
            return PathError::InvalidCharacter;
        }
    }

    const char last = component.Data()[component.Size() - 1];

    if (last == '.' || last == ' ')
    {
        return PathError::TrailingDotOrSpace;
    }

    if (IsReservedName(component))
    {
        return PathError::ReservedName;
    }

    return PathError::None;
}

/// Length of the root of a canonical path: 3 for "C:\", the length of
/// "\\server\share" for UNC, 0 if `path` is not canonical-looking.
Size CanonicalRootLength(StringView path) noexcept
{
    if (path.Size() >= 3 && IsAsciiLetter(path.Data()[0]) && path.Data()[1] == ':' &&
        path.Data()[2] == '\\')
    {
        return 3;
    }

    if (path.Size() >= 2 && path.Data()[0] == '\\' && path.Data()[1] == '\\')
    {
        Size i = 2;
        int separators = 0;

        while (i < path.Size())
        {
            if (path.Data()[i] == '\\' && ++separators == 2)
            {
                return i;
            }

            ++i;
        }

        return separators == 1 ? path.Size() : 0;
    }

    return 0;
}

/// Splits data[begin, end) on '\', appending non-empty components.
bool AppendComponents(const char* data, Size begin, Size end,
                      forge::core::Vector<StringView>& parts) noexcept
{
    Size start = begin;

    for (Size i = begin; i <= end; ++i)
    {
        if (i == end || data[i] == '\\')
        {
            if (i > start && parts.PushBack(StringView(data + start, i - start)).HasError())
            {
                return false;
            }

            start = i + 1;
        }
    }

    return true;
}

} // namespace

const char* DescribePathError(
    PathError error) noexcept
{
    switch (error)
    {
    case PathError::None: return "no error";
    case PathError::Empty: return "the path is empty";
    case PathError::ContainsNul: return "the path contains a NUL character";
    case PathError::InvalidCharacter:
        return "the path contains a character Windows does not allow in names (< > \" | ? * or a "
               "control character)";
    case PathError::DevicePath:
        return "device and namespace paths (\\\\?\\, \\\\.\\, \\??\\) are not allowed";
    case PathError::DriveRelative:
        return "drive-relative paths such as \"C:file\" are not allowed; write \"C:\\file\"";
    case PathError::InvalidUnc: return "the network path has no server or share name";
    case PathError::EscapesRoot: return "\"..\" goes above the root of the drive or share";
    case PathError::TrailingDotOrSpace:
        return "a name ends with a dot or a space, which Windows silently removes";
    case PathError::AlternateStream:
        return "\":\" inside a name (an NTFS alternate data stream) is not allowed";
    case PathError::ReservedName:
        return "the path uses a reserved device name (CON, NUL, COM1, LPT1, ...)";
    case PathError::TooLong: return "the path is too long";
    case PathError::ShortName:
        return "a name looks like an 8.3 short name (such as PROGRA~1) that does not exist; use "
               "the long name";
    }

    return "the path is not valid";
}

PathError LexicalCanonicalize(
    StringView input,
    StringView cwd,
    CanonicalPath& out) noexcept
{
    out.text.Clear();
    out.componentCount = 0;
    out.lastShortNameComponent = 0;
    out.isUnc = false;

    if (input.Empty())
    {
        return PathError::Empty;
    }

    if (input.Size() > kMaxPathBytes)
    {
        return PathError::TooLong;
    }

    // Work on a copy with '/' turned into '\'.
    String text;

    if (text.Reserve(input.Size()).HasError())
    {
        return PathError::TooLong;
    }

    for (Size i = 0; i < input.Size(); ++i)
    {
        const char c = input.Data()[i];

        if (c == '\0')
        {
            return PathError::ContainsNul;
        }

        if (text.Append(c == '/' ? '\\' : c).HasError())
        {
            return PathError::TooLong;
        }
    }

    const char* data = text.Data();
    const Size size = text.Size();

    if (size >= 4 && data[0] == '\\' &&
        ((data[1] == '\\' && (data[2] == '?' || data[2] == '.') && data[3] == '\\') ||
         (data[1] == '?' && data[2] == '?' && data[3] == '\\')))
    {
        return PathError::DevicePath;
    }

    // Root: "\\server\share", "X:\", "\" (cwd's root) or none (relative).
    String root;
    forge::core::Vector<StringView> parts;
    Size restBegin = 0;

    if (size >= 2 && data[0] == '\\' && data[1] == '\\')
    {
        Size serverEnd = 2;

        while (serverEnd < size && data[serverEnd] != '\\')
        {
            ++serverEnd;
        }

        Size shareEnd = serverEnd + 1;

        while (shareEnd < size && data[shareEnd] != '\\')
        {
            ++shareEnd;
        }

        if (serverEnd == 2 || serverEnd >= size || shareEnd == serverEnd + 1)
        {
            return PathError::InvalidUnc;
        }

        const StringView server(data + 2, serverEnd - 2);
        const StringView share(data + serverEnd + 1, shareEnd - serverEnd - 1);

        if (CheckComponent(server) != PathError::None || CheckComponent(share) != PathError::None ||
            server == StringView(".") || server == StringView("..") ||
            share == StringView(".") || share == StringView(".."))
        {
            return PathError::InvalidUnc;
        }

        if (root.Append(StringView(data, shareEnd)).HasError())
        {
            return PathError::TooLong;
        }

        out.isUnc = true;
        restBegin = shareEnd;
    }
    else if (size >= 2 && IsAsciiLetter(data[0]) && data[1] == ':')
    {
        if (size == 2 || data[2] != '\\')
        {
            return PathError::DriveRelative;
        }

        if (root.Append(FoldAscii(data[0])).HasError() || root.Append(":\\").HasError())
        {
            return PathError::TooLong;
        }

        restBegin = 3;
    }
    else
    {
        // Relative to cwd, or (leading '\') to cwd's root.
        const Size cwdRoot = CanonicalRootLength(cwd);

        if (cwdRoot == 0)
        {
            return PathError::DriveRelative;  // no usable base directory
        }

        if (root.Append(StringView(cwd.Data(), cwdRoot)).HasError())
        {
            return PathError::TooLong;
        }

        out.isUnc = cwd.Data()[0] == '\\';

        if (data[0] == '\\')
        {
            restBegin = 1;
        }
        else if (!AppendComponents(cwd.Data(), cwdRoot, cwd.Size(), parts))
        {
            return PathError::TooLong;
        }
    }

    // The input's own components, with "." and ".." applied.
    forge::core::Vector<StringView> inputParts;

    if (!AppendComponents(data, restBegin, size, inputParts))
    {
        return PathError::TooLong;
    }

    for (Size i = 0; i < inputParts.Size(); ++i)
    {
        const StringView part = inputParts[i];

        if (part == StringView("."))
        {
            continue;
        }

        if (part == StringView(".."))
        {
            if (parts.Empty())
            {
                return PathError::EscapesRoot;
            }

            parts.PopBack();
            continue;
        }

        const PathError error = CheckComponent(part);

        if (error != PathError::None)
        {
            return error;
        }

        if (parts.PushBack(part).HasError())
        {
            return PathError::TooLong;
        }
    }

    // Assemble. `root` is "X:\" or "\\server\share" (no trailing '\').
    String result;

    if (result.Append(root.View()).HasError())
    {
        return PathError::TooLong;
    }

    for (Size i = 0; i < parts.Size(); ++i)
    {
        const bool needSeparator = !(result.Size() > 0 && result.Data()[result.Size() - 1] == '\\');

        if ((needSeparator && result.Append('\\').HasError()) ||
            result.Append(parts[i]).HasError())
        {
            return PathError::TooLong;
        }

        if (LooksLikeShortName(parts[i]))
        {
            out.lastShortNameComponent = i + 1;
        }
    }

    if (result.Size() > kMaxPathBytes)
    {
        return PathError::TooLong;
    }

    out.componentCount = parts.Size();
    out.text.Swap(result);
    return PathError::None;
}

bool PathEquals(
    StringView a,
    StringView b) noexcept
{
    return EqualsIgnoreAsciiCase(a, b);
}

bool PathCovers(
    StringView grant,
    StringView path) noexcept
{
    if (grant.Empty() || path.Size() < grant.Size())
    {
        return false;
    }

    if (!EqualsIgnoreAsciiCase(grant, StringView(path.Data(), grant.Size())))
    {
        return false;
    }

    if (path.Size() == grant.Size())
    {
        return true;
    }

    // A drive root already ends in '\'; anything else needs one next.
    return grant.Data()[grant.Size() - 1] == '\\' || path.Data()[grant.Size()] == '\\';
}

bool IsRootPath(
    StringView canonical) noexcept
{
    const Size rootLength = CanonicalRootLength(canonical);
    return rootLength != 0 && rootLength == canonical.Size();
}

bool HasComponent(
    StringView canonical,
    StringView name) noexcept
{
    const Size rootLength = CanonicalRootLength(canonical);
    Size start = rootLength;

    for (Size i = rootLength; i <= canonical.Size(); ++i)
    {
        if (i == canonical.Size() || canonical.Data()[i] == '\\')
        {
            if (i > start && EqualsIgnoreAsciiCase(StringView(canonical.Data() + start, i - start), name))
            {
                return true;
            }

            start = i + 1;
        }
    }

    return false;
}

StringView LastComponent(
    StringView canonical) noexcept
{
    const Size rootLength = CanonicalRootLength(canonical);

    if (rootLength == 0 || rootLength >= canonical.Size())
    {
        return StringView();
    }

    Size i = canonical.Size();

    while (i > rootLength && canonical.Data()[i - 1] != '\\')
    {
        --i;
    }

    return StringView(canonical.Data() + i, canonical.Size() - i);
}

StringView ParentPath(
    StringView canonical) noexcept
{
    const Size rootLength = CanonicalRootLength(canonical);

    if (rootLength == 0 || rootLength >= canonical.Size())
    {
        return canonical;
    }

    Size i = canonical.Size();

    while (i > rootLength && canonical.Data()[i - 1] != '\\')
    {
        --i;
    }

    // i is just after the separator before the last component.
    if (i <= rootLength)
    {
        // Directly under the root: "C:\a" -> "C:\", "\\s\sh\a" -> "\\s\sh".
        return StringView(canonical.Data(), rootLength);
    }

    return StringView(canonical.Data(), i - 1);
}

} // namespace forge::kavach
