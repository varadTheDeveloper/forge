#include "kavach/SecureFs.h"

#include <stddef.h>
#include <string.h>

#include <utility>

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "forge-core/platform/Unicode.h"
#include "forge-core/platform/Win32Error.h"

namespace forge::kavach::fs
{

namespace
{

using forge::core::Error;
using forge::core::ErrorCode;
using forge::core::Failure;
using forge::core::Result;
using forge::core::Size;
using forge::core::String;
using forge::core::StringView;
using forge::core::u8;
using forge::core::u64;
using forge::core::Vector;
using forge::core::platform::TranslateWin32Error;

//------------------------------------------------------------------------------
// The few native NT definitions needed for relative opens. Declared here
// rather than taken from winternl.h, whose contents differ between SDKs; the
// layouts are the documented, stable ones.
//------------------------------------------------------------------------------

using NtStatus = LONG;

struct NtUnicodeString
{
    USHORT Length;         // bytes, without a terminator
    USHORT MaximumLength;  // bytes
    PWSTR Buffer;
};

struct NtObjectAttributes
{
    ULONG Length;
    HANDLE RootDirectory;
    NtUnicodeString* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct NtIoStatusBlock
{
    union
    {
        NtStatus Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};

using NtCreateFileFn = NtStatus(NTAPI*)(PHANDLE, ACCESS_MASK, NtObjectAttributes*, NtIoStatusBlock*,
                                        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(NtStatus);
using NtSetInformationFileFn = NtStatus(NTAPI*)(HANDLE, NtIoStatusBlock*, PVOID, ULONG, int);

// FILE_INFORMATION_CLASS for NtSetInformationFile.
constexpr int kFileRenameInformationEx = 65;

constexpr ULONG kObjCaseInsensitive = 0x00000040;
constexpr ULONG kFileOpen = 1;
constexpr ULONG kFileCreate = 2;
constexpr ULONG kFileOpenIf = 3;
constexpr ULONG kFileDirectoryFile = 0x00000001;
constexpr ULONG kFileSynchronousIoNonAlert = 0x00000020;
constexpr ULONG kFileNonDirectoryFile = 0x00000040;
constexpr ULONG kFileOpenForBackupIntent = 0x00004000;
constexpr ULONG kFileOpenReparsePoint = 0x00200000;

// FILE_INFO_BY_HANDLE_CLASS values (stable since Windows 10 1709; Forge
// requires Windows 11, decision D7).
constexpr int kFileDispositionInfoEx = 21;
constexpr DWORD kDispositionDelete = 0x1;
constexpr DWORD kDispositionPosix = 0x2;
constexpr DWORD kDispositionIgnoreReadOnly = 0x10;
constexpr DWORD kRenameReplaceIfExists = 0x1;
constexpr DWORD kRenamePosix = 0x2;

// IO_REPARSE_TAG_* for links.
constexpr DWORD kReparseTagSymlink = 0xA000000C;
constexpr DWORD kReparseTagMountPoint = 0xA0000003;

struct NtApi
{
    NtCreateFileFn createFile = nullptr;
    RtlNtStatusToDosErrorFn toDosError = nullptr;
    NtSetInformationFileFn setInformationFile = nullptr;
};

const NtApi& Nt() noexcept
{
    // ntdll.dll is mapped into every process before main(); no search.
    static const NtApi api = []() {
        NtApi result;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");

        if (ntdll != nullptr)
        {
            // Through void*: FARPROC has no parameters, so a direct cast
            // between function types draws a warning.
            result.createFile = reinterpret_cast<NtCreateFileFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")));
            result.toDosError = reinterpret_cast<RtlNtStatusToDosErrorFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
            result.setInformationFile = reinterpret_cast<NtSetInformationFileFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "NtSetInformationFile")));
        }

        return result;
    }();

    return api;
}

Error NtError(NtStatus status) noexcept
{
    const NtApi& api = Nt();
    const ULONG code = api.toDosError ? api.toDosError(status) : ERROR_GEN_FAILURE;
    return TranslateWin32Error(code);
}

Error LastError() noexcept
{
    return TranslateWin32Error(GetLastError());
}

/// Closes a handle when it goes out of scope.
class Handle
{
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : handle_(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : handle_(other.handle_)
    {
        other.handle_ = INVALID_HANDLE_VALUE;
    }

    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }

        return *this;
    }

    ~Handle() { Reset(); }

    HANDLE Get() const noexcept { return handle_; }
    bool Valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

    void Reset() noexcept
    {
        if (Valid())
        {
            CloseHandle(handle_);
        }

        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

template <typename T>
Result<T> Fail(Error error) noexcept
{
    return Result<T>(Failure{ error });
}

template <typename T>
Result<T> Refused(const CheckedPath& path, Denial& denial, Denial::Kind kind,
                  StringView target) noexcept
{
    RecordLateDenial(path, denial, kind, target);
    return Fail<T>(Error(ErrorCode::PermissionDenied));
}

/// "C:\a" -> "\\?\C:\a", "\\s\sh\a" -> "\\?\UNC\s\sh\a".
Result<Vector<wchar_t>> ToNtWide(StringView canonical) noexcept
{
    String prefixed;
    const bool unc = canonical.Size() >= 2 && canonical.Data()[0] == '\\';
    const bool ok =
        unc ? (prefixed.Append("\\\\?\\UNC\\").HasValue() &&
               prefixed.Append(StringView(canonical.Data() + 2, canonical.Size() - 2)).HasValue())
            : (prefixed.Append("\\\\?\\").HasValue() && prefixed.Append(canonical).HasValue());

    if (!ok)
    {
        return Fail<Vector<wchar_t>>(Error(ErrorCode::OutOfMemory));
    }

    return forge::core::platform::Utf8ToUtf16(prefixed.View());
}

/// The handle's final path in Kavach's canonical form.
Result<String> FinalPathOf(HANDLE handle) noexcept
{
    wchar_t stackBuffer[512];
    Vector<wchar_t> heapBuffer;
    wchar_t* buffer = stackBuffer;
    DWORD capacity = static_cast<DWORD>(sizeof(stackBuffer) / sizeof(stackBuffer[0]));
    DWORD length =
        GetFinalPathNameByHandleW(handle, buffer, capacity, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);

    if (length >= capacity)
    {
        if (heapBuffer.Reserve(static_cast<Size>(length) + 1).HasError())
        {
            return Fail<String>(Error(ErrorCode::OutOfMemory));
        }

        for (DWORD i = 0; i <= length; ++i)
        {
            heapBuffer.PushBack(L'\0').Ignore();
        }

        buffer = heapBuffer.Data();
        capacity = length + 1;
        length = GetFinalPathNameByHandleW(handle, buffer, capacity,
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }

    if (length == 0 || length >= capacity)
    {
        return Fail<String>(LastError());
    }

    String utf8;

    if (forge::core::platform::AppendUtf16AsUtf8(buffer, length, utf8).HasError())
    {
        return Fail<String>(Error(ErrorCode::OutOfMemory));
    }

    const StringView view = utf8.View();
    String plain;

    if (view.StartsWith("\\\\?\\UNC\\"))
    {
        plain.Append("\\\\").Ignore();
        plain.Append(StringView(view.Data() + 8, view.Size() - 8)).Ignore();
    }
    else if (view.StartsWith("\\\\?\\"))
    {
        plain.Append(StringView(view.Data() + 4, view.Size() - 4)).Ignore();
    }
    else
    {
        return Fail<String>(Error(ErrorCode::InvalidData));
    }

    CanonicalPath canonical;

    if (LexicalCanonicalize(plain.View(), StringView(), canonical) != PathError::None)
    {
        return Fail<String>(Error(ErrorCode::InvalidData));
    }

    return Result<String>(std::move(canonical.text));
}

/// Opens a folder by its canonical path, following links.
Result<Handle> OpenDirectoryByPath(StringView canonical) noexcept
{
    Result<Vector<wchar_t>> wide = ToNtWide(canonical);

    if (wide.HasError())
    {
        return Fail<Handle>(wide.Error());
    }

    HANDLE handle = CreateFileW(wide.Value().Data(),
                                FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        return Fail<Handle>(LastError());
    }

    return Result<Handle>(Handle(handle));
}

/// Opens `name` (one component) relative to the folder `root`.
Result<Handle> OpenRelative(HANDLE root, StringView name, ACCESS_MASK access, ULONG disposition,
                            ULONG options) noexcept
{
    const NtApi& api = Nt();

    if (api.createFile == nullptr)
    {
        return Fail<Handle>(Error(ErrorCode::NotSupported));
    }

    Result<Vector<wchar_t>> wide = forge::core::platform::Utf8ToUtf16(name);

    if (wide.HasError())
    {
        return Fail<Handle>(wide.Error());
    }

    const Size units = wide.Value().Size() - 1;  // without the terminator

    if (units == 0 || units * sizeof(wchar_t) > 0xFFFE)
    {
        return Fail<Handle>(Error(ErrorCode::InvalidArgument));
    }

    NtUnicodeString objectName;
    objectName.Length = static_cast<USHORT>(units * sizeof(wchar_t));
    objectName.MaximumLength = objectName.Length;
    objectName.Buffer = wide.Value().Data();

    NtObjectAttributes attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = root;
    attributes.ObjectName = &objectName;
    attributes.Attributes = kObjCaseInsensitive;

    NtIoStatusBlock io{};
    HANDLE handle = nullptr;
    const NtStatus status = api.createFile(
        &handle, access | SYNCHRONIZE, &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition,
        options | kFileSynchronousIoNonAlert | kFileOpenForBackupIntent, nullptr, 0);

    if (status < 0)
    {
        return Fail<Handle>(NtError(status));
    }

    return Result<Handle>(Handle(handle));
}

struct HandleInfo
{
    DWORD attributes = 0;
    DWORD reparseTag = 0;
    u64 size = 0;
    DWORD links = 0;
    bool directory = false;
    LARGE_INTEGER creation{};
    LARGE_INTEGER lastAccess{};
    LARGE_INTEGER lastWrite{};
    LARGE_INTEGER change{};
};

Result<HandleInfo> QueryHandle(HANDLE handle) noexcept
{
    HandleInfo info;
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    FILE_ATTRIBUTE_TAG_INFO tag{};

    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ||
        !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)))
    {
        return Fail<HandleInfo>(LastError());
    }

    info.attributes = basic.FileAttributes;
    info.reparseTag = (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? tag.ReparseTag : 0;
    info.size = static_cast<u64>(standard.EndOfFile.QuadPart);
    info.links = standard.NumberOfLinks;
    info.directory = standard.Directory != FALSE;
    info.creation = basic.CreationTime;
    info.lastAccess = basic.LastAccessTime;
    info.lastWrite = basic.LastWriteTime;
    info.change = basic.ChangeTime;
    return Result<HandleInfo>(info);
}

bool IsLink(const HandleInfo& info) noexcept
{
    return info.reparseTag == kReparseTagSymlink || info.reparseTag == kReparseTagMountPoint;
}

double ToEpochMs(LARGE_INTEGER time) noexcept
{
    // 100 ns intervals since 1601-01-01 -> milliseconds since 1970-01-01.
    constexpr long long kEpochDifference = 116444736000000000LL;
    return static_cast<double>(time.QuadPart - kEpochDifference) / 10000.0;
}

Stat ToStat(const HandleInfo& info) noexcept
{
    Stat stat;
    stat.size = info.directory ? 0 : info.size;
    stat.links = info.links;
    stat.type = IsLink(info) ? EntryType::Symlink
              : info.directory ? EntryType::Directory
                               : EntryType::File;
    stat.mtimeMs = ToEpochMs(info.lastWrite);
    stat.atimeMs = ToEpochMs(info.lastAccess);
    stat.ctimeMs = ToEpochMs(info.change);
    stat.birthtimeMs = ToEpochMs(info.creation);
    stat.readOnly = (info.attributes & FILE_ATTRIBUTE_READONLY) != 0;
    return stat;
}

/// Opens the folder that contains `path` and makes sure it is exactly the
/// folder that was checked. `name` receives the last component.
Result<Handle> OpenCheckedParent(const CheckedPath& path, StringView& name,
                                 Denial& denial) noexcept
{
    const StringView text = path.Text();

    if (IsRootPath(text))
    {
        return Fail<Handle>(Error(ErrorCode::InvalidOperation));
    }

    const StringView parentPath = ParentPath(text);
    name = LastComponent(text);
    Result<Handle> parent = OpenDirectoryByPath(parentPath);

    if (parent.HasError())
    {
        return parent;
    }

    Result<String> real = FinalPathOf(parent.Value().Get());

    if (real.HasError())
    {
        return Fail<Handle>(real.Error());
    }

    if (!PathEquals(real.Value().View(), parentPath))
    {
        return Refused<Handle>(path, denial, Denial::Kind::Changed, real.Value().View());
    }

    return parent;
}

/// After opening (and possibly following links), the handle must still
/// lead somewhere the check allows.
Result<void> VerifyHandle(const CheckedPath& path, HANDLE handle, Denial& denial) noexcept
{
    Result<String> real = FinalPathOf(handle);

    if (real.HasError())
    {
        return Fail<void>(real.Error());
    }

    if (!VerifyOpenedPath(path, real.Value().View(), denial))
    {
        return Fail<void>(Error(ErrorCode::PermissionDenied));
    }

    return Result<void>();
}

/// Opens the checked path for reading, following links, and verifies it.
Result<Handle> OpenForRead(const CheckedPath& path, ACCESS_MASK access, ULONG options,
                           Denial& denial) noexcept
{
    StringView name;
    Result<Handle> parent = OpenCheckedParent(path, name, denial);

    if (parent.HasError())
    {
        return parent;
    }

    Result<Handle> file = OpenRelative(parent.Value().Get(), name, access, kFileOpen, options);

    if (file.HasError())
    {
        return file;
    }

    if (Result<void> verified = VerifyHandle(path, file.Value().Get(), denial); verified.HasError())
    {
        return Fail<Handle>(verified.Error());
    }

    return file;
}

/// Opens (creating if needed) the checked path for writing without
/// following a link, and refuses links and multiply-linked files.
Result<Handle> OpenForWrite(const CheckedPath& path, WriteMode mode, Denial& denial) noexcept
{
    StringView name;
    Result<Handle> parent = OpenCheckedParent(path, name, denial);

    if (parent.HasError())
    {
        return parent;
    }

    // Append opens get FILE_APPEND_DATA only, so every write lands at the end.
    const ACCESS_MASK access =
        (mode == WriteMode::Append ? FILE_APPEND_DATA : (FILE_WRITE_DATA | FILE_APPEND_DATA)) |
        FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES;
    Result<Handle> file = OpenRelative(parent.Value().Get(), name, access, kFileOpenIf,
                                       kFileNonDirectoryFile | kFileOpenReparsePoint);

    if (file.HasError())
    {
        return file;
    }

    Result<HandleInfo> info = QueryHandle(file.Value().Get());

    if (info.HasError())
    {
        return Fail<Handle>(info.Error());
    }

    if (info.Value().reparseTag != 0)
    {
        return Refused<Handle>(path, denial, Denial::Kind::Link, path.Text());
    }

    if (info.Value().links > 1)
    {
        return Refused<Handle>(path, denial, Denial::Kind::HardLink, path.Text());
    }

    if (Result<void> verified = VerifyHandle(path, file.Value().Get(), denial); verified.HasError())
    {
        return Fail<Handle>(verified.Error());
    }

    if (mode == WriteMode::Truncate)
    {
        FILE_END_OF_FILE_INFO end{};

        if (!SetFileInformationByHandle(file.Value().Get(), FileEndOfFileInfo, &end, sizeof(end)))
        {
            return Fail<Handle>(LastError());
        }
    }

    return file;
}

Result<void> WriteAll(HANDLE handle, const u8* data, Size size) noexcept
{
    constexpr Size kChunk = Size{ 1 } << 30;
    Size offset = 0;

    while (offset < size)
    {
        const Size remaining = size - offset;
        const DWORD request = static_cast<DWORD>(remaining > kChunk ? kChunk : remaining);
        DWORD written = 0;

        if (!::WriteFile(handle, data + offset, request, &written, nullptr))
        {
            return Fail<void>(LastError());
        }

        if (written == 0)
        {
            return Fail<void>(Error(ErrorCode::IOError));
        }

        offset += written;
    }

    return Result<void>();
}

Result<void> MarkDeleted(HANDLE handle) noexcept
{
    struct
    {
        DWORD Flags;
    } disposition{ kDispositionDelete | kDispositionPosix | kDispositionIgnoreReadOnly };

    if (!SetFileInformationByHandle(handle, static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileDispositionInfoEx),
                                    &disposition, sizeof(disposition)))
    {
        return Fail<void>(LastError());
    }

    return Result<void>();
}

/// Lists a folder through its handle; "." and ".." are left out.
Result<Vector<DirEntry>> ListHandle(HANDLE directory) noexcept
{
    Vector<DirEntry> entries;
    Vector<u8> buffer;
    constexpr Size kBufferSize = 64 * 1024;

    if (buffer.Reserve(kBufferSize).HasError())
    {
        return Fail<Vector<DirEntry>>(Error(ErrorCode::OutOfMemory));
    }

    for (Size i = 0; i < kBufferSize; ++i)
    {
        buffer.PushBack(0).Ignore();
    }

    bool first = true;

    for (;;)
    {
        const FILE_INFO_BY_HANDLE_CLASS infoClass =
            first ? FileFullDirectoryRestartInfo : FileFullDirectoryInfo;
        first = false;

        if (!GetFileInformationByHandleEx(directory, infoClass, buffer.Data(),
                                          static_cast<DWORD>(buffer.Size())))
        {
            const DWORD error = GetLastError();

            if (error == ERROR_NO_MORE_FILES)
            {
                break;
            }

            return Fail<Vector<DirEntry>>(TranslateWin32Error(error));
        }

        Size offset = 0;

        for (;;)
        {
            const auto* entry = reinterpret_cast<const FILE_FULL_DIR_INFO*>(buffer.Data() + offset);
            const Size units = entry->FileNameLength / sizeof(wchar_t);
            const bool dot = units == 1 && entry->FileName[0] == L'.';
            const bool dotDot = units == 2 && entry->FileName[0] == L'.' && entry->FileName[1] == L'.';

            if (!dot && !dotDot)
            {
                DirEntry item;

                if (forge::core::platform::AppendUtf16AsUtf8(entry->FileName, units, item.name)
                        .HasError())
                {
                    return Fail<Vector<DirEntry>>(Error(ErrorCode::OutOfMemory));
                }

                // For a reparse point, EaSize holds the reparse tag.
                const bool reparse = (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                const DWORD tag = reparse ? entry->EaSize : 0;

                if (tag == kReparseTagSymlink || tag == kReparseTagMountPoint)
                {
                    item.type = EntryType::Symlink;
                }
                else if (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    item.type = EntryType::Directory;
                }
                else
                {
                    item.type = EntryType::File;
                }

                if (entries.PushBack(std::move(item)).HasError())
                {
                    return Fail<Vector<DirEntry>>(Error(ErrorCode::OutOfMemory));
                }
            }

            if (entry->NextEntryOffset == 0)
            {
                break;
            }

            offset += entry->NextEntryOffset;
        }
    }

    return Result<Vector<DirEntry>>(std::move(entries));
}

/// Deletes everything inside `directory`, never descending into links.
Result<void> RemoveContents(HANDLE directory, int depth) noexcept
{
    if (depth > 256)
    {
        return Fail<void>(Error(ErrorCode::Overflow));
    }

    Result<Vector<DirEntry>> entries = ListHandle(directory);

    if (entries.HasError())
    {
        return Fail<void>(entries.Error());
    }

    for (Size i = 0; i < entries.Value().Size(); ++i)
    {
        const DirEntry& entry = entries.Value()[i];
        Result<Handle> child =
            OpenRelative(directory, entry.name.View(),
                         DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                         kFileOpen, kFileOpenReparsePoint);

        if (child.HasError())
        {
            return Fail<void>(child.Error());
        }

        Result<HandleInfo> info = QueryHandle(child.Value().Get());

        if (info.HasError())
        {
            return Fail<void>(info.Error());
        }

        if (info.Value().directory && info.Value().reparseTag == 0)
        {
            if (Result<void> removed = RemoveContents(child.Value().Get(), depth + 1);
                removed.HasError())
            {
                return removed;
            }
        }

        if (Result<void> deleted = MarkDeleted(child.Value().Get()); deleted.HasError())
        {
            return deleted;
        }
    }

    return Result<void>();
}

} // namespace

Result<Vector<u8>> ReadFile(const CheckedPath& path, Denial& denial) noexcept
{
    denial = Denial();
    Result<Handle> file =
        OpenForRead(path, FILE_READ_DATA | FILE_READ_ATTRIBUTES, kFileNonDirectoryFile, denial);

    if (file.HasError())
    {
        return Fail<Vector<u8>>(file.Error());
    }

    Vector<u8> bytes;
    LARGE_INTEGER size{};

    // Read straight into a buffer of the file's size; Vector has no resize,
    // so it is grown to that size first (the capacity is reserved).
    if (GetFileSizeEx(file.Value().Get(), &size) && size.QuadPart > 0)
    {
        if (static_cast<unsigned long long>(size.QuadPart) > (1ULL << 31))
        {
            return Fail<Vector<u8>>(Error(ErrorCode::Overflow));  // larger than a JS typed array
        }

        const Size expected = static_cast<Size>(size.QuadPart);

        if (bytes.Reserve(expected).HasError())
        {
            return Fail<Vector<u8>>(Error(ErrorCode::OutOfMemory));
        }

        for (Size i = 0; i < expected; ++i)
        {
            bytes.PushBack(0).Ignore();
        }

        Size filled = 0;

        while (filled < expected)
        {
            DWORD read = 0;
            const Size remaining = expected - filled;
            const DWORD request = static_cast<DWORD>(remaining > (1u << 30) ? (1u << 30) : remaining);

            if (!::ReadFile(file.Value().Get(), bytes.Data() + filled, request, &read, nullptr))
            {
                return Fail<Vector<u8>>(LastError());
            }

            if (read == 0)
            {
                break;  // the file shrank
            }

            filled += read;
        }

        while (bytes.Size() > filled)
        {
            bytes.PopBack();
        }
    }

    // Anything appended while reading (or a file whose size was unknown).
    u8 chunk[64 * 1024];

    for (;;)
    {
        DWORD read = 0;

        if (!::ReadFile(file.Value().Get(), chunk, sizeof(chunk), &read, nullptr))
        {
            return Fail<Vector<u8>>(LastError());
        }

        if (read == 0)
        {
            break;
        }

        for (DWORD i = 0; i < read; ++i)
        {
            if (bytes.PushBack(chunk[i]).HasError())
            {
                return Fail<Vector<u8>>(Error(ErrorCode::OutOfMemory));
            }
        }
    }

    return Result<Vector<u8>>(std::move(bytes));
}

Result<void> WriteFile(const CheckedPath& path, forge::core::Span<const u8> data, WriteMode mode,
                       Denial& denial) noexcept
{
    denial = Denial();
    Result<Handle> file = OpenForWrite(path, mode, denial);

    if (file.HasError())
    {
        return Fail<void>(file.Error());
    }

    return WriteAll(file.Value().Get(), data.Data(), data.Size());
}

Result<bool> Exists(const CheckedPath& path, Denial& denial) noexcept
{
    denial = Denial();

    if (IsRootPath(path.Text()))
    {
        Result<Handle> root = OpenDirectoryByPath(path.Text());
        return Result<bool>(root.HasValue());
    }

    Result<Handle> file = OpenForRead(path, FILE_READ_ATTRIBUTES, 0, denial);

    if (file.HasError())
    {
        if (file.Error() == ErrorCode::NotFound)
        {
            return Result<bool>(false);
        }

        return Fail<bool>(file.Error());
    }

    return Result<bool>(true);
}

Result<Stat> GetStat(const CheckedPath& path, bool followLinks, Denial& denial) noexcept
{
    denial = Denial();
    Result<Handle> file = Fail<Handle>(Error(ErrorCode::Unknown));

    if (IsRootPath(path.Text()))
    {
        file = OpenDirectoryByPath(path.Text());
    }
    else if (followLinks)
    {
        file = OpenForRead(path, FILE_READ_ATTRIBUTES, 0, denial);
    }
    else
    {
        StringView name;
        Result<Handle> parent = OpenCheckedParent(path, name, denial);

        if (parent.HasError())
        {
            return Fail<Stat>(parent.Error());
        }

        file = OpenRelative(parent.Value().Get(), name, FILE_READ_ATTRIBUTES, kFileOpen,
                            kFileOpenReparsePoint);
    }

    if (file.HasError())
    {
        return Fail<Stat>(file.Error());
    }

    Result<HandleInfo> info = QueryHandle(file.Value().Get());

    if (info.HasError())
    {
        return Fail<Stat>(info.Error());
    }

    return Result<Stat>(ToStat(info.Value()));
}

Result<String> RealPath(const CheckedPath& path, Denial& denial) noexcept
{
    denial = Denial();
    Result<Handle> file = IsRootPath(path.Text()) ? OpenDirectoryByPath(path.Text())
                                                  : OpenForRead(path, FILE_READ_ATTRIBUTES, 0, denial);

    if (file.HasError())
    {
        return Fail<String>(file.Error());
    }

    return FinalPathOf(file.Value().Get());
}

Result<Vector<DirEntry>> ReadDirectory(const CheckedPath& path, Denial& denial) noexcept
{
    denial = Denial();
    Result<Handle> directory = Fail<Handle>(Error(ErrorCode::Unknown));

    if (IsRootPath(path.Text()))
    {
        directory = OpenDirectoryByPath(path.Text());
    }
    else
    {
        directory = OpenForRead(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, kFileDirectoryFile,
                                denial);
    }

    if (directory.HasError())
    {
        return Fail<Vector<DirEntry>>(directory.Error());
    }

    return ListHandle(directory.Value().Get());
}

Result<void> MakeDirectory(const CheckedPath& path, bool recursive, Denial& denial) noexcept
{
    denial = Denial();
    const ACCESS_MASK access = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;

    if (!recursive)
    {
        StringView name;
        Result<Handle> parent = OpenCheckedParent(path, name, denial);

        if (parent.HasError())
        {
            return Fail<void>(parent.Error());
        }

        Result<Handle> created = OpenRelative(parent.Value().Get(), name, access, kFileCreate,
                                              kFileDirectoryFile | kFileOpenReparsePoint);
        return created.HasError() ? Fail<void>(created.Error()) : Result<void>();
    }

    // Find the deepest folder that already exists, check that it is exactly
    // where the check said, then create each missing level relative to the
    // handle of the level above it.
    StringView prefix = path.Text();
    Vector<StringView> missing;  // deepest first
    Handle current;

    for (;;)
    {
        Result<Handle> opened = OpenDirectoryByPath(prefix);

        if (opened.HasValue())
        {
            Result<String> real = FinalPathOf(opened.Value().Get());

            if (real.HasError())
            {
                return Fail<void>(real.Error());
            }

            if (!PathEquals(real.Value().View(), prefix))
            {
                return Refused<void>(path, denial, Denial::Kind::Changed, real.Value().View());
            }

            current = std::move(opened.Value());
            break;
        }

        if (!(opened.Error() == ErrorCode::NotFound) || IsRootPath(prefix))
        {
            // Something exists but is not a folder, or the drive is gone.
            return Fail<void>(opened.Error() == ErrorCode::NotFound ? opened.Error()
                                                                    : Error(ErrorCode::AlreadyExists));
        }

        if (missing.PushBack(LastComponent(prefix)).HasError())
        {
            return Fail<void>(Error(ErrorCode::OutOfMemory));
        }

        prefix = ParentPath(prefix);
    }

    for (Size i = missing.Size(); i > 0; --i)
    {
        Result<Handle> next = OpenRelative(current.Get(), missing[i - 1], access, kFileOpenIf,
                                           kFileDirectoryFile | kFileOpenReparsePoint);

        if (next.HasError())
        {
            return Fail<void>(next.Error());
        }

        Result<HandleInfo> info = QueryHandle(next.Value().Get());

        if (info.HasError())
        {
            return Fail<void>(info.Error());
        }

        if (info.Value().reparseTag != 0)
        {
            return Refused<void>(path, denial, Denial::Kind::Link, path.Text());
        }

        current = std::move(next.Value());
    }

    return Result<void>();
}

Result<void> Remove(const CheckedPath& path, bool recursive, Denial& denial) noexcept
{
    denial = Denial();
    StringView name;
    Result<Handle> parent = OpenCheckedParent(path, name, denial);

    if (parent.HasError())
    {
        return Fail<void>(parent.Error());
    }

    Result<Handle> target =
        OpenRelative(parent.Value().Get(), name,
                     DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                     kFileOpen, kFileOpenReparsePoint);

    if (target.HasError())
    {
        return Fail<void>(target.Error());
    }

    Result<HandleInfo> info = QueryHandle(target.Value().Get());

    if (info.HasError())
    {
        return Fail<void>(info.Error());
    }

    if (info.Value().directory && info.Value().reparseTag == 0)
    {
        if (!recursive)
        {
            return Fail<void>(Error(ErrorCode::InvalidOperation));
        }

        if (Result<void> removed = RemoveContents(target.Value().Get(), 0); removed.HasError())
        {
            return removed;
        }
    }

    return MarkDeleted(target.Value().Get());
}

Result<void> Rename(const CheckedPath& from, const CheckedPath& to, Denial& denial) noexcept
{
    denial = Denial();
    StringView fromName;
    Result<Handle> fromParent = OpenCheckedParent(from, fromName, denial);

    if (fromParent.HasError())
    {
        return Fail<void>(fromParent.Error());
    }

    StringView toName;
    Result<Handle> toParent = OpenCheckedParent(to, toName, denial);

    if (toParent.HasError())
    {
        return Fail<void>(toParent.Error());
    }

    Result<Handle> source = OpenRelative(fromParent.Value().Get(), fromName,
                                         DELETE | FILE_READ_ATTRIBUTES, kFileOpen,
                                         kFileOpenReparsePoint);

    if (source.HasError())
    {
        return Fail<void>(source.Error());
    }

    Result<Vector<wchar_t>> wideName = forge::core::platform::Utf8ToUtf16(toName);

    if (wideName.HasError())
    {
        return Fail<void>(wideName.Error());
    }

    // FILE_RENAME_INFORMATION_EX, passed straight to NtSetInformationFile
    // so the name is taken relative to the destination folder's handle with
    // no Win32 path conversion. FileName starts right after FileNameLength
    // (offset 20 on x64), not at the end of the padded header: the first
    // version of this code copied it to offset 24 and Windows rejected the
    // name with ERROR_INVALID_NAME.
    struct RenameInformation
    {
        ULONG Flags;
        HANDLE RootDirectory;
        ULONG FileNameLength;  // bytes
        WCHAR FileName[1];
    };

    const NtApi& api = Nt();

    if (api.setInformationFile == nullptr)
    {
        return Fail<void>(Error(ErrorCode::NotSupported));
    }

    const Size nameBytes = (wideName.Value().Size() - 1) * sizeof(wchar_t);
    const Size total = offsetof(RenameInformation, FileName) + nameBytes + sizeof(wchar_t);
    Vector<u8> buffer;

    if (buffer.Reserve(total).HasError())
    {
        return Fail<void>(Error(ErrorCode::OutOfMemory));
    }

    for (Size i = 0; i < total; ++i)
    {
        buffer.PushBack(0).Ignore();
    }

    auto* info = reinterpret_cast<RenameInformation*>(buffer.Data());
    info->Flags = kRenameReplaceIfExists | kRenamePosix;
    info->RootDirectory = toParent.Value().Get();
    info->FileNameLength = static_cast<ULONG>(nameBytes);
    memcpy(buffer.Data() + offsetof(RenameInformation, FileName), wideName.Value().Data(), nameBytes);

    NtIoStatusBlock io{};
    const NtStatus status = api.setInformationFile(source.Value().Get(), &io, buffer.Data(),
                                                   static_cast<ULONG>(total),
                                                   kFileRenameInformationEx);

    if (status < 0)
    {
        return Fail<void>(NtError(status));
    }

    return Result<void>();
}

Result<void> CopyFile(const CheckedPath& from, const CheckedPath& to, Denial& denial) noexcept
{
    denial = Denial();
    Result<Handle> source =
        OpenForRead(from, FILE_READ_DATA | FILE_READ_ATTRIBUTES, kFileNonDirectoryFile, denial);

    if (source.HasError())
    {
        return Fail<void>(source.Error());
    }

    Result<Handle> destination = OpenForWrite(to, WriteMode::Truncate, denial);

    if (destination.HasError())
    {
        return Fail<void>(destination.Error());
    }

    Vector<u8> chunk;
    constexpr Size kChunk = 1024 * 1024;

    if (chunk.Reserve(kChunk).HasError())
    {
        return Fail<void>(Error(ErrorCode::OutOfMemory));
    }

    for (Size i = 0; i < kChunk; ++i)
    {
        chunk.PushBack(0).Ignore();
    }

    for (;;)
    {
        DWORD read = 0;

        if (!::ReadFile(source.Value().Get(), chunk.Data(), static_cast<DWORD>(kChunk), &read,
                        nullptr))
        {
            return Fail<void>(LastError());
        }

        if (read == 0)
        {
            return Result<void>();
        }

        if (Result<void> written = WriteAll(destination.Value().Get(), chunk.Data(), read);
            written.HasError())
        {
            return written;
        }
    }
}

} // namespace forge::kavach::fs
