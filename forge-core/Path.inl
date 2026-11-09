#pragma once

#include <utility>

namespace forge::core
{

//==============================================================================
// Helpers
//==============================================================================

inline bool Path::IsAsciiAlpha(
    char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

inline Path::SizeType Path::SplitRoot(
    StringView text,
    StringView* prefix,
    bool* rooted) noexcept
{
    const SizeType size = text.Size();

    // Index just past the component starting at `from` (stops at '/' or
    // the end of the text).
    auto componentEnd = [&text, size](SizeType from) noexcept -> SizeType
    {
        SizeType end = from;

        while (end < size && text[end] != '/')
        {
            ++end;
        }

        return end;
    };

    // Consumes one separator after a prefix that ends at `end`, if any.
    auto finish = [&text, size, prefix](SizeType end) noexcept -> SizeType
    {
        *prefix = text.Substr(0, end);
        return (end < size && text[end] == '/') ? end + 1 : end;
    };

    if (size >= 2 && text[0] == '/' && text[1] == '/')
    {
        *rooted = true;

        // Device paths: "//?/..." (Win32 \\?\) and "//./..." (\\.\).
        if (size >= 4 && (text[2] == '?' || text[2] == '.') && text[3] == '/')
        {
            SizeType end = componentEnd(4); // volume or device: "C:", "PIPE", "UNC"
            const StringView first = text.Substr(4, end - 4);

            const bool isUnc = first.Size() == 3 &&
                               (first[0] == 'U' || first[0] == 'u') &&
                               (first[1] == 'N' || first[1] == 'n') &&
                               (first[2] == 'C' || first[2] == 'c');

            if (isUnc && end < size)
            {
                end = componentEnd(end + 1); // server

                if (end < size)
                {
                    end = componentEnd(end + 1); // share
                }
            }

            return finish(end);
        }

        // UNC: "//server/share". A bare "//" (no server) is its own root.
        if (size == 2 || text[2] == '/')
        {
            *prefix = text.Substr(0, 2);
            return 2;
        }

        SizeType end = componentEnd(2); // server

        if (end < size)
        {
            end = componentEnd(end + 1); // share
        }

        return finish(end);
    }

    if (size >= 2 && IsAsciiAlpha(text[0]) && text[1] == ':')
    {
        *rooted = size >= 3 && text[2] == '/';
        return finish(2);
    }

    *prefix = StringView();

    if (size >= 1 && text[0] == '/')
    {
        *rooted = true;
        return 1;
    }

    *rooted = false;
    return 0;
}

inline bool Path::IsRootPath(
    StringView text) noexcept
{
    if (text.Empty())
    {
        return false;
    }

    StringView prefix;
    bool rooted = false;
    const SizeType consumed = SplitRoot(text, &prefix, &rooted);

    return rooted && consumed == text.Size();
}

//==============================================================================
// Construction
//==============================================================================

inline Path::Path() noexcept
    :
    value_()
{
}

inline Path::Path(
    memory::Allocator& allocator) noexcept
    :
    value_(allocator)
{
}

inline Result<Path> Path::Create(
    memory::Allocator& allocator,
    StringView text)
{
    Path path(allocator);

    if (Result<void> reserved = path.value_.Reserve(text.Size()); reserved.HasError())
    {
        return Result<Path>(Failure{ reserved.Error() });
    }

    for (Size index = 0; index < text.Size(); ++index)
    {
        char c = text[index];

        if (c == '\\')
        {
            c = '/';
        }

        if (Result<void> appended = path.value_.Append(c); appended.HasError())
        {
            return Result<Path>(Failure{ appended.Error() });
        }
    }

    return Result<Path>(std::move(path));
}

inline Result<Path> Path::Create(
    StringView text)
{
    return Create(memory::GetDefaultAllocator(), text);
}

//==============================================================================
// Observers
//==============================================================================

inline StringView Path::View() const noexcept
{
    return value_.View();
}

inline bool Path::Empty() const noexcept
{
    return value_.Empty();
}

inline bool Path::IsAbsolute() const noexcept
{
    StringView v = value_.View();

    if (v.Size() >= 3 && IsAsciiAlpha(v[0]) && v[1] == ':' && v[2] == '/')
    {
        return true;
    }

    if (v.Size() >= 2 && v[0] == '/' && v[1] == '/')
    {
        return true;
    }

    return false;
}

inline bool Path::IsRelative() const noexcept
{
    return !IsAbsolute();
}

inline StringView Path::Parent() const noexcept
{
    StringView v = value_.View();

    if (v.Empty() || IsRootPath(v))
    {
        return v;
    }

    SizeType end = v.Size();

    if (v[end - 1] == '/')
    {
        --end;
    }

    StringView trimmed = v.Substr(0, end);

    if (trimmed.Empty() || IsRootPath(trimmed))
    {
        return trimmed;
    }

    SizeType lastSlash = trimmed.RFind(StringView("/"));

    if (lastSlash == StringView::kNotFound)
    {
        return StringView(); // no separator at all -> no parent
    }

    StringView candidateParent = trimmed.Substr(0, lastSlash + 1);

    if (IsRootPath(candidateParent))
    {
        return candidateParent; // keep the root's own trailing slash
    }

    return trimmed.Substr(0, lastSlash);
}

inline StringView Path::FileName() const noexcept
{
    StringView v = value_.View();

    if (v.Empty() || IsRootPath(v))
    {
        return StringView();
    }

    SizeType end = v.Size();

    if (v[end - 1] == '/')
    {
        --end;
    }

    StringView trimmed = v.Substr(0, end);

    if (trimmed.Empty() || IsRootPath(trimmed))
    {
        return StringView();
    }

    SizeType lastSlash = trimmed.RFind(StringView("/"));

    if (lastSlash == StringView::kNotFound)
    {
        return trimmed;
    }

    return trimmed.Substr(lastSlash + 1);
}

inline StringView Path::Stem() const noexcept
{
    StringView name = FileName();
    SizeType dot = name.RFind(StringView("."));

    if (dot == StringView::kNotFound || dot == 0)
    {
        return name;
    }

    return name.Substr(0, dot);
}

inline StringView Path::Extension() const noexcept
{
    StringView name = FileName();
    SizeType dot = name.RFind(StringView("."));

    if (dot == StringView::kNotFound || dot == 0)
    {
        return StringView();
    }

    return name.Substr(dot);
}

//==============================================================================
// Modifiers
//==============================================================================

inline Result<void> Path::Append(
    StringView segment)
{
    if (segment.Empty())
    {
        return {};
    }

    const bool needSeparator = !value_.Empty() && value_.View()[value_.Size() - 1] != '/';

    if (needSeparator)
    {
        if (Result<void> result = value_.Append('/'); result.HasError())
        {
            return result;
        }
    }

    for (Size index = 0; index < segment.Size(); ++index)
    {
        char c = segment[index];

        if (c == '\\')
        {
            c = '/';
        }

        if (Result<void> result = value_.Append(c); result.HasError())
        {
            return result;
        }
    }

    return {};
}

inline Result<Path> Path::Join(
    memory::Allocator& allocator,
    StringView base,
    StringView segment)
{
    Result<Path> path = Path::Create(allocator, base);

    if (path.HasError())
    {
        return path;
    }

    if (Result<void> result = path.Value().Append(segment); result.HasError())
    {
        return Result<Path>(Failure{ result.Error() });
    }

    return path;
}

inline Result<Path> Path::Join(
    StringView base,
    StringView segment)
{
    return Join(memory::GetDefaultAllocator(), base, segment);
}

inline Result<void> Path::Normalize()
{
    StringView v = value_.View();

    if (v.Empty())
    {
        return {};
    }

    // Phase 7.9: the root is split off first and copied through
    // unchanged, so ".." can never climb above a UNC share or a device
    // volume, and a drive-relative path keeps its drive letter. Before
    // this, "//server/share/x" lost a slash (becoming "/server/share/x",
    // no longer absolute) and "//server/share/../../x" became "/x".
    StringView prefix;
    bool rooted = false;
    const SizeType consumed = SplitRoot(v, &prefix, &rooted);
    const bool separatorAfterPrefix = consumed > prefix.Size();
    StringView rest = v.Substr(consumed);

    Vector<StringView> stack; // resolved segments, aliasing the ORIGINAL
                               // buffer — safe as long as nothing mutates
                               // value_ until after this loop finishes.

    SizeType index = 0;

    while (index <= rest.Size())
    {
        SizeType next = rest.Size();

        for (SizeType scan = index; scan < rest.Size(); ++scan)
        {
            if (rest[scan] == '/')
            {
                next = scan;
                break;
            }
        }

        StringView segment = rest.Substr(index, next - index);

        if (!segment.Empty() && segment != StringView("."))
        {
            if (segment == StringView(".."))
            {
                if (!stack.Empty() && stack.Back() != StringView(".."))
                {
                    stack.PopBack();
                }
                else if (!rooted)
                {
                    if (Result<void> result = stack.PushBack(segment); result.HasError())
                    {
                        return result;
                    }
                }
                // else: rooted path, ".." above the root is dropped.
            }
            else
            {
                if (Result<void> result = stack.PushBack(segment); result.HasError())
                {
                    return result;
                }
            }
        }

        index = next + 1;
    }

    // Build the result into a completely separate scratch String (its
    // own, temporary default-allocator buffer) rather than rewriting
    // value_ in place — `stack`'s StringViews still alias value_'s
    // CURRENT buffer, so mutating value_ (which could reallocate) while
    // still reading from `stack` would be a use-after-free. Copy-
    // assigning the finished result into value_ at the very end (which
    // copies bytes using value_'s OWN allocator, per String's own
    // copy-assignment semantics) means value_'s original allocator is
    // preserved throughout, even though `rebuilt` itself used the
    // default one.
    String rebuilt;

    if (Result<void> result = rebuilt.Append(prefix); result.HasError())
    {
        return result;
    }

    // One separator between the root and the first segment. "/" and
    // "C:/" always keep theirs; a UNC or device root keeps its trailing
    // separator only if the input had one or segments follow. The bare
    // "//" prefix already ends in a separator.
    const bool prefixEndsInSeparator = !prefix.Empty() && prefix[prefix.Size() - 1] == '/';
    const bool needSeparator = rooted && !prefixEndsInSeparator &&
                               (prefix.Empty() || separatorAfterPrefix || !stack.Empty());

    if (needSeparator)
    {
        if (Result<void> result = rebuilt.Append('/'); result.HasError())
        {
            return result;
        }
    }

    // A relative path whose first remaining segment looks like a drive
    // ("./C:/x", or "b/../C:x") must keep a leading "./", or dropping the
    // "." / ".." would turn it into a drive path (Phase 7.9, found by the
    // "Normalize preserves IsAbsolute()" property).
    if (prefix.Empty() && !rooted && !stack.Empty())
    {
        const StringView first = stack[0];

        if (first.Size() >= 2 && IsAsciiAlpha(first[0]) && first[1] == ':')
        {
            if (Result<void> result = rebuilt.Append(StringView("./")); result.HasError())
            {
                return result;
            }
        }
    }

    for (Size i = 0; i < stack.Size(); ++i)
    {
        if (i > 0)
        {
            if (Result<void> result = rebuilt.Append('/'); result.HasError())
            {
                return result;
            }
        }

        if (Result<void> result = rebuilt.Append(stack[i]); result.HasError())
        {
            return result;
        }
    }

    if (rebuilt.Empty())
    {
        // A relative path that fully normalized away, e.g. "." or "a/..".
        if (Result<void> result = rebuilt.Append(StringView(".")); result.HasError())
        {
            return result;
        }
    }

    value_ = rebuilt;

    return {};
}

inline void Path::Swap(
    Path& other) noexcept
{
    value_.Swap(other.value_);
}

inline Path::operator StringView() const noexcept
{
    return View();
}

} // namespace forge::core
