#include "Unicode.h"

#include <climits>
#include <utility>

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace forge::core::platform
{

Result<Vector<wchar_t>> Utf8ToUtf16(
    StringView text) noexcept
{
    Vector<wchar_t> wide;

    if (text.Size() > static_cast<Size>(INT_MAX))
    {
        return Result<Vector<wchar_t>>(Failure{ Error(ErrorCode::InvalidArgument) });
    }

    int required = 0;

    if (!text.Empty())
    {
        required = MultiByteToWideChar(
            CP_UTF8, 0, text.Data(), static_cast<int>(text.Size()), nullptr, 0);

        if (required <= 0)
        {
            return Result<Vector<wchar_t>>(
                Failure{ Error(ErrorCode::InvalidData, static_cast<i32>(GetLastError())) });
        }
    }

    if (Result<void> reserved = wide.Reserve(static_cast<Size>(required) + 1);
        reserved.HasError())
    {
        return Result<Vector<wchar_t>>(Failure{ reserved.Error() });
    }

    // Vector has no "resize": grow it to the final length (the capacity is
    // already reserved, so this cannot fail), then convert in place.
    for (int i = 0; i <= required; ++i)
    {
        if (Result<void> pushed = wide.PushBack(L'\0'); pushed.HasError())
        {
            return Result<Vector<wchar_t>>(Failure{ pushed.Error() });
        }
    }

    if (required > 0)
    {
        const int converted = MultiByteToWideChar(
            CP_UTF8, 0, text.Data(), static_cast<int>(text.Size()), wide.Data(), required);

        if (converted != required)
        {
            return Result<Vector<wchar_t>>(
                Failure{ Error(ErrorCode::InvalidData, static_cast<i32>(GetLastError())) });
        }
    }

    return Result<Vector<wchar_t>>(std::move(wide));
}

Result<void> AppendUtf16AsUtf8(
    const wchar_t* text,
    Size length,
    String& out) noexcept
{
    // Convert in pieces so the int-sized Win32 lengths can never overflow
    // and the scratch buffer stays on the stack. A piece never ends between
    // the two halves of a surrogate pair.
    constexpr Size kPiece = 2048;
    char buffer[kPiece * 3];

    Size offset = 0;

    while (offset < length)
    {
        Size count = length - offset;

        if (count > kPiece)
        {
            count = kPiece;
            const wchar_t last = text[offset + count - 1];

            if (last >= 0xD800 && last <= 0xDBFF)
            {
                --count;  // keep a high surrogate with its low half
            }
        }

        const int written = WideCharToMultiByte(
            CP_UTF8, 0, text + offset, static_cast<int>(count), buffer,
            static_cast<int>(sizeof(buffer)), nullptr, nullptr);

        if (written <= 0)
        {
            return Result<void>(
                Failure{ Error(ErrorCode::InvalidData, static_cast<i32>(GetLastError())) });
        }

        if (Result<void> appended =
                out.Append(StringView(buffer, static_cast<Size>(written)));
            appended.HasError())
        {
            return appended;
        }

        offset += count;
    }

    return Result<void>();
}

} // namespace forge::core::platform
