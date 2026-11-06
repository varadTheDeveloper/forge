// Win32 implementation of File — see File.md. Cannot be compiled in the
// sandbox this was written in (no Windows SDK / working MinGW cross
// compiler available there); verified by manual review only. Needs a
// real `mach build` (or a standalone Visual Studio smoke test, matching
// IocpSmokeTest.cpp's precedent) to go from "implemented" to "confirmed
// working" — flagged explicitly per AGENTS.md's "Be Honest".

#if !defined(_WIN32)
#error "forge::core::File: no backend implemented for this platform (Win32 only, matching forge::core::platform::IoLoop's own precedent)."
#endif

#include "File.h"

#include <utility>

#include <windows.h>

#include "platform/Win32Error.h"

namespace forge::core
{

namespace
{

// TranslateWin32Error used to have its own private copy of this switch
// right here. Phase 4 (Mutex/ConditionVariable/Thread) needed the exact
// same ERROR_* -> ErrorCode mapping, so it's now shared via
// platform::TranslateWin32Error (platform/Win32Error.h/.cpp) instead of
// becoming a third independent copy — see HISTORY.md's Phase 4 entry.
using platform::TranslateWin32Error;

/// UTF-8 (Path's internal encoding) -> UTF-16, null-terminated — the
/// only string form every Win32 API used below accepts. Vector<T> has no
/// bulk "resize and give me a writable pointer" operation, so this grows
/// the buffer one element at a time up to the required length and then
/// lets MultiByteToWideChar fill it in place via Data() — the same
/// pattern ReadAllBytes() below uses for the same reason.
Result<Vector<wchar_t>> Utf8ToWide(
    StringView text) noexcept
{
    Vector<wchar_t> wide;

    if (text.Empty())
    {
        if (Result<void> appended = wide.PushBack(L'\0'); appended.HasError())
        {
            return Result<Vector<wchar_t>>(Failure{ appended.Error() });
        }

        return Result<Vector<wchar_t>>(std::move(wide));
    }

    const int required = MultiByteToWideChar(
        CP_UTF8, 0, text.Data(), static_cast<int>(text.Size()), nullptr, 0);

    if (required <= 0)
    {
        return Result<Vector<wchar_t>>(
            Failure{ Error(ErrorCode::InvalidData, static_cast<i32>(GetLastError())) });
    }

    if (Result<void> reserved = wide.Reserve(static_cast<Size>(required) + 1); reserved.HasError())
    {
        return Result<Vector<wchar_t>>(Failure{ reserved.Error() });
    }

    for (int i = 0; i < required; ++i)
    {
        if (Result<void> appended = wide.PushBack(L'\0'); appended.HasError())
        {
            return Result<Vector<wchar_t>>(Failure{ appended.Error() });
        }
    }

    MultiByteToWideChar(
        CP_UTF8, 0, text.Data(), static_cast<int>(text.Size()), wide.Data(), required);

    if (Result<void> appended = wide.PushBack(L'\0'); appended.HasError()) // null terminator
    {
        return Result<Vector<wchar_t>>(Failure{ appended.Error() });
    }

    return Result<Vector<wchar_t>>(std::move(wide));
}

} // namespace

//==============================================================================
// Construction
//==============================================================================

File::File() noexcept
    :
    handle_(nullptr)
{
}

File::File(
    void* handle) noexcept
    :
    handle_(handle)
{
}

File::File(
    File&& other) noexcept
    :
    handle_(other.handle_)
{
    other.handle_ = nullptr;
}

File& File::operator=(
    File&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    Close();

    handle_ = other.handle_;
    other.handle_ = nullptr;

    return *this;
}

File::~File() noexcept
{
    Close();
}

//==============================================================================
// Open / Close
//==============================================================================

Result<File> File::Open(
    const Path& path,
    FileMode mode)
{
    Result<Vector<wchar_t>> wide = Utf8ToWide(path.View());

    if (wide.HasError())
    {
        return Result<File>(Failure{ wide.Error() });
    }

    DWORD access = 0;
    DWORD disposition = 0;

    switch (mode)
    {
        case FileMode::Read:
            access = GENERIC_READ;
            disposition = OPEN_EXISTING;
            break;

        case FileMode::Write:
            access = GENERIC_WRITE;
            disposition = CREATE_ALWAYS;
            break;

        case FileMode::ReadWrite:
            access = GENERIC_READ | GENERIC_WRITE;
            disposition = CREATE_ALWAYS;
            break;

        case FileMode::Append:
            access = GENERIC_WRITE;
            disposition = OPEN_ALWAYS;
            break;

        case FileMode::CreateNew:
            access = GENERIC_READ | GENERIC_WRITE;
            disposition = CREATE_NEW;
            break;
    }

    HANDLE handle = CreateFileW(
        wide.Value().Data(),
        access,
        FILE_SHARE_READ,
        nullptr,
        disposition,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        return Result<File>(Failure{ TranslateWin32Error(GetLastError()) });
    }

    if (mode == FileMode::Append)
    {
        // Position at end-of-file once, up front. Nothing else in this
        // class moves the file pointer on its own between Write() calls,
        // so a single seek here (rather than reseeking before every
        // Write()) is enough to get "every write appends" behavior for
        // the documented write-only Append use case.
        LARGE_INTEGER zero{};
        zero.QuadPart = 0;

        if (!SetFilePointerEx(handle, zero, nullptr, FILE_END))
        {
            const Error seekError = TranslateWin32Error(GetLastError());
            CloseHandle(handle);
            return Result<File>(Failure{ seekError });
        }
    }

    return Result<File>(File(static_cast<void*>(handle)));
}

void File::Close() noexcept
{
    if (handle_ != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

bool File::IsOpen() const noexcept
{
    return handle_ != nullptr;
}

//==============================================================================
// Read / Write / Seek
//==============================================================================

Result<File::SizeType> File::Read(
    Span<u8> buffer)
{
    if (handle_ == nullptr)
    {
        return Result<SizeType>(Failure{ Error(ErrorCode::InvalidOperation) });
    }

    if (buffer.Empty())
    {
        return Result<SizeType>(SizeType{ 0 });
    }

    DWORD bytesRead = 0;

    // ReadFile returning TRUE with bytesRead == 0 means end-of-file —
    // deliberately not treated as an error, per File.md.
    if (!ReadFile(
            static_cast<HANDLE>(handle_),
            buffer.Data(),
            static_cast<DWORD>(buffer.Size()),
            &bytesRead,
            nullptr))
    {
        return Result<SizeType>(Failure{ TranslateWin32Error(GetLastError()) });
    }

    return Result<SizeType>(static_cast<SizeType>(bytesRead));
}

Result<File::SizeType> File::Write(
    Span<const u8> buffer)
{
    if (handle_ == nullptr)
    {
        return Result<SizeType>(Failure{ Error(ErrorCode::InvalidOperation) });
    }

    if (buffer.Empty())
    {
        return Result<SizeType>(SizeType{ 0 });
    }

    DWORD bytesWritten = 0;

    if (!WriteFile(
            static_cast<HANDLE>(handle_),
            buffer.Data(),
            static_cast<DWORD>(buffer.Size()),
            &bytesWritten,
            nullptr))
    {
        return Result<SizeType>(Failure{ TranslateWin32Error(GetLastError()) });
    }

    return Result<SizeType>(static_cast<SizeType>(bytesWritten));
}

Result<u64> File::Seek(
    i64 offset,
    SeekOrigin origin)
{
    if (handle_ == nullptr)
    {
        return Result<u64>(Failure{ Error(ErrorCode::InvalidOperation) });
    }

    DWORD method = FILE_BEGIN;

    switch (origin)
    {
        case SeekOrigin::Begin:   method = FILE_BEGIN;   break;
        case SeekOrigin::Current: method = FILE_CURRENT; break;
        case SeekOrigin::End:     method = FILE_END;     break;
    }

    LARGE_INTEGER distance{};
    distance.QuadPart = offset;

    LARGE_INTEGER newPosition{};

    if (!SetFilePointerEx(static_cast<HANDLE>(handle_), distance, &newPosition, method))
    {
        return Result<u64>(Failure{ TranslateWin32Error(GetLastError()) });
    }

    return Result<u64>(static_cast<u64>(newPosition.QuadPart));
}

Result<u64> File::Tell()
{
    return Seek(0, SeekOrigin::Current);
}

Result<u64> File::SizeInBytes()
{
    if (handle_ == nullptr)
    {
        return Result<u64>(Failure{ Error(ErrorCode::InvalidOperation) });
    }

    LARGE_INTEGER size{};

    if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &size))
    {
        return Result<u64>(Failure{ TranslateWin32Error(GetLastError()) });
    }

    return Result<u64>(static_cast<u64>(size.QuadPart));
}

//==============================================================================
// Path-only operations
//==============================================================================

Result<bool> File::Exists(
    const Path& path)
{
    Result<Vector<wchar_t>> wide = Utf8ToWide(path.View());

    if (wide.HasError())
    {
        return Result<bool>(Failure{ wide.Error() });
    }

    const DWORD attributes = GetFileAttributesW(wide.Value().Data());

    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD lastError = GetLastError();

        if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND)
        {
            return Result<bool>(false);
        }

        return Result<bool>(Failure{ TranslateWin32Error(lastError) });
    }

    return Result<bool>(true);
}

Result<void> File::MakeDirectory(
    const Path& path)
{
    Result<Vector<wchar_t>> wide = Utf8ToWide(path.View());

    if (wide.HasError())
    {
        return Result<void>(Failure{ wide.Error() });
    }

    if (!CreateDirectoryW(wide.Value().Data(), nullptr))
    {
        return Result<void>(Failure{ TranslateWin32Error(GetLastError()) });
    }

    return {};
}

Result<void> File::CreateDirectories(
    const Path& path)
{
    if (path.Empty())
    {
        return {};
    }

    // Walk from `path` up to the root via repeated Parent() calls,
    // collecting each ancestor as an owned Path (NOT a StringView into
    // `current` — `current` gets reassigned every iteration, which would
    // leave any StringView into its old buffer dangling). Then create
    // them shallowest-first.
    Vector<Path> ancestors;

    Path current = path;

    while (!current.Empty())
    {
        Result<Path> currentCopy = Path::Create(current.View());

        if (currentCopy.HasError())
        {
            return Result<void>(Failure{ currentCopy.Error() });
        }

        if (Result<void> pushed = ancestors.PushBack(std::move(currentCopy.Value())); pushed.HasError())
        {
            return pushed;
        }

        StringView parentView = current.Parent();

        if (parentView.Empty() || parentView == current.View())
        {
            break; // ran out of separators, or reached a root (its own parent)
        }

        Result<Path> parentResult = Path::Create(parentView);

        if (parentResult.HasError())
        {
            return Result<void>(Failure{ parentResult.Error() });
        }

        current = std::move(parentResult.Value());
    }

    for (Size i = ancestors.Size(); i > 0; --i)
    {
        Result<void> created = MakeDirectory(ancestors[i - 1]);

        if (created.HasError() && !(created.Error() == ErrorCode::AlreadyExists))
        {
            return created;
        }
    }

    return {};
}

Result<void> File::Remove(
    const Path& path)
{
    Result<Vector<wchar_t>> wide = Utf8ToWide(path.View());

    if (wide.HasError())
    {
        return Result<void>(Failure{ wide.Error() });
    }

    if (DeleteFileW(wide.Value().Data()))
    {
        return {};
    }

    DWORD lastError = GetLastError();

    // DeleteFileW on a directory fails with ERROR_ACCESS_DENIED; retry
    // via RemoveDirectoryW rather than probing attributes first (avoids
    // a check-then-act race against whatever `path` actually is).
    if (lastError == ERROR_ACCESS_DENIED)
    {
        if (RemoveDirectoryW(wide.Value().Data()))
        {
            return {};
        }

        lastError = GetLastError();
    }

    return Result<void>(Failure{ TranslateWin32Error(lastError) });
}

//==============================================================================
// Convenience wrappers
//==============================================================================

Result<Vector<u8>> File::ReadAllBytes(
    const Path& path)
{
    Result<File> opened = Open(path, FileMode::Read);

    if (opened.HasError())
    {
        return Result<Vector<u8>>(Failure{ opened.Error() });
    }

    File file = std::move(opened.Value());

    Result<u64> size = file.SizeInBytes();

    if (size.HasError())
    {
        return Result<Vector<u8>>(Failure{ size.Error() });
    }

    Vector<u8> bytes;

    if (Result<void> reserved = bytes.Reserve(static_cast<Size>(size.Value())); reserved.HasError())
    {
        return Result<Vector<u8>>(Failure{ reserved.Error() });
    }

    for (u64 i = 0; i < size.Value(); ++i)
    {
        if (Result<void> appended = bytes.PushBack(u8{ 0 }); appended.HasError())
        {
            return Result<Vector<u8>>(Failure{ appended.Error() });
        }
    }

    Size totalRead = 0;

    while (totalRead < bytes.Size())
    {
        Span<u8> remaining(bytes.Data() + totalRead, bytes.Size() - totalRead);
        Result<Size> readResult = file.Read(remaining);

        if (readResult.HasError())
        {
            return Result<Vector<u8>>(Failure{ readResult.Error() });
        }

        if (readResult.Value() == 0)
        {
            break; // file shrank since SizeInBytes(), or the size was
                    // otherwise inaccurate -- stop rather than loop
                    // forever on a Read() that keeps returning 0.
        }

        totalRead += readResult.Value();
    }

    // SizeInBytes() is only the initial size estimate. If the file reached
    // EOF before filling the buffer, discard the zero-initialized tail so the
    // returned Vector reflects the bytes actually read.
    while (bytes.Size() > totalRead)
    {
        bytes.PopBack();
    }

    return Result<Vector<u8>>(std::move(bytes));
}

Result<String> File::ReadAllText(
    const Path& path)
{
    Result<Vector<u8>> bytes = ReadAllBytes(path);

    if (bytes.HasError())
    {
        return Result<String>(Failure{ bytes.Error() });
    }

    const StringView text(
        reinterpret_cast<const char*>(bytes.Value().Data()),
        bytes.Value().Size());

    return String::Create(text);
}

} // namespace forge::core
