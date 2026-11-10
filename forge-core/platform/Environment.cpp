#include "Environment.h"

#include <utility>

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Unicode.h"
#include "Win32Error.h"

namespace forge::core::platform
{

bool IsValidEnvName(
    StringView name) noexcept
{
    if (name.Empty())
    {
        return false;
    }

    for (Size i = 0; i < name.Size(); ++i)
    {
        if (name.Data()[i] == '=' || name.Data()[i] == '\0')
        {
            return false;
        }
    }

    return true;
}

Result<bool> GetEnv(
    StringView name,
    String& value) noexcept
{
    value.Clear();

    if (!IsValidEnvName(name))
    {
        return Result<bool>(Failure{ Error(ErrorCode::InvalidArgument) });
    }

    Result<Vector<wchar_t>> wideName = Utf8ToUtf16(name);

    if (wideName.HasError())
    {
        return Result<bool>(Failure{ wideName.Error() });
    }

    // Variables can change size between calls (another thread may set
    // them), so retry until the buffer is big enough.
    Vector<wchar_t> buffer;
    DWORD capacity = 256;

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        buffer.Clear();

        if (Result<void> reserved = buffer.Reserve(capacity); reserved.HasError())
        {
            return Result<bool>(Failure{ reserved.Error() });
        }

        for (DWORD i = 0; i < capacity; ++i)
        {
            if (Result<void> pushed = buffer.PushBack(L'\0'); pushed.HasError())
            {
                return Result<bool>(Failure{ pushed.Error() });
            }
        }

        SetLastError(ERROR_SUCCESS);
        const DWORD length =
            GetEnvironmentVariableW(wideName.Value().Data(), buffer.Data(), capacity);

        if (length == 0)
        {
            const DWORD error = GetLastError();

            if (error == ERROR_ENVVAR_NOT_FOUND)
            {
                return Result<bool>(false);
            }

            if (error == ERROR_SUCCESS)
            {
                return Result<bool>(true);  // set, but empty
            }

            return Result<bool>(Failure{ TranslateWin32Error(error) });
        }

        if (length < capacity)
        {
            if (Result<void> converted = AppendUtf16AsUtf8(buffer.Data(), length, value);
                converted.HasError())
            {
                return Result<bool>(Failure{ converted.Error() });
            }

            return Result<bool>(true);
        }

        capacity = length;  // the size needed, including the terminator
    }

    return Result<bool>(Failure{ Error(ErrorCode::Busy) });
}

Result<Vector<EnvEntry>> ListEnv() noexcept
{
    Vector<EnvEntry> entries;
    wchar_t* block = GetEnvironmentStringsW();

    if (block == nullptr)
    {
        return Result<Vector<EnvEntry>>(Failure{ TranslateWin32Error(GetLastError()) });
    }

    Result<void> status;

    for (const wchar_t* entry = block; *entry != L'\0';)
    {
        Size length = 0;

        while (entry[length] != L'\0')
        {
            ++length;
        }

        // "NAME=VALUE". Names starting with '=' are the hidden per-drive
        // current directories ("=C:=C:\\work"); skip them.
        if (entry[0] != L'=')
        {
            Size equals = 0;

            while (equals < length && entry[equals] != L'=')
            {
                ++equals;
            }

            EnvEntry item;
            status = AppendUtf16AsUtf8(entry, equals, item.name);

            if (status.HasValue() && equals < length)
            {
                status = AppendUtf16AsUtf8(entry + equals + 1, length - equals - 1, item.value);
            }

            if (status.HasValue())
            {
                status = entries.PushBack(std::move(item));
            }

            if (status.HasError())
            {
                break;
            }
        }

        entry += length + 1;
    }

    FreeEnvironmentStringsW(block);

    if (status.HasError())
    {
        return Result<Vector<EnvEntry>>(Failure{ status.Error() });
    }

    return Result<Vector<EnvEntry>>(std::move(entries));
}

} // namespace forge::core::platform
