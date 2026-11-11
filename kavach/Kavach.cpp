#include "kavach/Kavach.h"

#include <stdio.h>

#include <utility>

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "forge-core/LockGuard.h"
#include "forge-core/Mutex.h"
#include "forge-core/platform/Environment.h"
#include "forge-core/platform/Unicode.h"

namespace forge::kavach
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
using forge::core::Vector;

/// Audit rate limit per op (R15): a burst of this many records, then this
/// many per second. Decisions beyond it are still made and enforced; they
/// are counted and written as one summary record.
constexpr double kAuditBurst = 1000;
constexpr double kAuditPerSecond = 100;

struct Grant
{
    String asTyped;   // canonical, before resolving links (lexical)
    String resolved;  // canonical, links and short names resolved
};

struct AuditBucket
{
    double tokens = kAuditBurst;
    ULONGLONG lastMs = 0;
    unsigned long long suppressedAllow = 0;
    unsigned long long suppressedDeny = 0;
};

struct State
{
    bool initialized = false;

    String cwd;
    String installDir;
    String homeDir;

    Vector<Grant> readGrants;
    Vector<Grant> writeGrants;
    Vector<String> envNames;
    bool allEnv = false;

    String auditPath;
    HANDLE audit = INVALID_HANDLE_VALUE;
    bool auditBroken = false;
    AuditBucket buckets[kOpCount];
};

State g_state;

// Phase 8.3: filesystem work runs on worker threads too, and they may need
// to record a late refusal. Every audit write holds this lock. The policy
// itself never changes after Initialize(), so reading it needs no lock.
forge::core::Mutex g_auditMutex;

/// The permission an audit record names: the access for filesystem ops,
/// otherwise the op's own permission.
const char* AuditPermissionName(Op op, Access access) noexcept
{
    const Permission permission = GetOpInfo(op).permission;

    if (permission == Permission::Read || permission == Permission::Write ||
        permission == Permission::ReadWrite)
    {
        return access == Access::Write ? "write" : "read";
    }

    return PermissionName(permission);
}

bool Append(String& out, StringView text) noexcept
{
    return out.Append(text).HasValue();
}

//------------------------------------------------------------------------------
// Resolving links, junctions and short names (Windows)
//------------------------------------------------------------------------------

/// "C:\a" -> "\\?\C:\a", "\\s\sh\a" -> "\\?\UNC\s\sh\a": the long-path form,
/// safe because a canonical path has no "." or "..".
bool ToNtStyleWide(StringView canonical, Vector<wchar_t>& wide) noexcept
{
    String prefixed;
    const bool unc = canonical.Size() >= 2 && canonical.Data()[0] == '\\';
    const bool ok = unc ? (Append(prefixed, "\\\\?\\UNC\\") &&
                           Append(prefixed, StringView(canonical.Data() + 2, canonical.Size() - 2)))
                        : (Append(prefixed, "\\\\?\\") && Append(prefixed, canonical));

    if (!ok)
    {
        return false;
    }

    Result<Vector<wchar_t>> converted = forge::core::platform::Utf8ToUtf16(prefixed.View());

    if (converted.HasError())
    {
        return false;
    }

    wide = std::move(converted.Value());
    return true;
}

enum class ProbeResult
{
    Resolved,
    Missing,  // does not exist (or cannot be opened at all)
    Failed,   // out of memory or an unexpected answer
};

/// If `canonical` exists, replaces `resolved` with Windows' final path for
/// it: links and junctions followed, short names expanded, on-disk case.
ProbeResult ProbeFinalPath(StringView canonical, String& resolved) noexcept
{
    Vector<wchar_t> wide;

    if (!ToNtStyleWide(canonical, wide))
    {
        return ProbeResult::Failed;
    }

    // No access rights are requested; FILE_FLAG_BACKUP_SEMANTICS lets this
    // open folders too. Links are followed (no FILE_FLAG_OPEN_REPARSE_POINT).
    HANDLE handle = CreateFileW(wide.Data(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        return ProbeResult::Missing;
    }

    wchar_t stackBuffer[512];
    Vector<wchar_t> heapBuffer;
    wchar_t* buffer = stackBuffer;
    DWORD capacity = static_cast<DWORD>(sizeof(stackBuffer) / sizeof(stackBuffer[0]));
    DWORD length = GetFinalPathNameByHandleW(handle, buffer, capacity,
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);

    if (length >= capacity)
    {
        if (heapBuffer.Reserve(static_cast<Size>(length) + 1).HasError())
        {
            CloseHandle(handle);
            return ProbeResult::Failed;
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

    CloseHandle(handle);

    if (length == 0 || length >= capacity)
    {
        return ProbeResult::Failed;
    }

    String utf8;

    if (forge::core::platform::AppendUtf16AsUtf8(buffer, length, utf8).HasError())
    {
        return ProbeResult::Failed;
    }

    // Strip "\\?\" or turn "\\?\UNC\" back into "\\".
    StringView view = utf8.View();
    String plain;

    if (view.StartsWith("\\\\?\\UNC\\"))
    {
        if (!Append(plain, "\\\\") || !Append(plain, StringView(view.Data() + 8, view.Size() - 8)))
        {
            return ProbeResult::Failed;
        }
    }
    else if (view.StartsWith("\\\\?\\"))
    {
        if (!Append(plain, StringView(view.Data() + 4, view.Size() - 4)))
        {
            return ProbeResult::Failed;
        }
    }
    else
    {
        return ProbeResult::Failed;  // a volume GUID path or something else unexpected
    }

    // Bring it into canonical form (upper-case drive letter, no trailing
    // '\'). A final path Windows hands back must canonicalize cleanly.
    CanonicalPath canonicalResult;

    if (LexicalCanonicalize(plain.View(), StringView(), canonicalResult) != PathError::None)
    {
        return ProbeResult::Failed;
    }

    resolved.Swap(canonicalResult.text);
    return ProbeResult::Resolved;
}

/// Resolves the longest existing prefix of `lexical` and appends the rest.
/// Fails with ShortName if a component that looks like an 8.3 name is in
/// the part that does not exist (it could be an alias of anything).
PathError Resolve(const CanonicalPath& lexical, String& out) noexcept
{
    out.Clear();
    StringView prefix = lexical.text.View();
    Vector<StringView> missing;  // last component first

    for (;;)
    {
        String resolvedPrefix;
        const ProbeResult probe = ProbeFinalPath(prefix, resolvedPrefix);

        if (probe == ProbeResult::Failed)
        {
            return PathError::TooLong;  // reported as an internal failure by the caller
        }

        const bool atRoot = IsRootPath(prefix);

        if (probe == ProbeResult::Resolved || atRoot)
        {
            const Size existingComponents = lexical.componentCount - missing.Size();

            if (lexical.lastShortNameComponent > existingComponents)
            {
                return PathError::ShortName;
            }

            if (!Append(out, probe == ProbeResult::Resolved ? resolvedPrefix.View() : prefix))
            {
                return PathError::TooLong;
            }

            for (Size i = missing.Size(); i > 0; --i)
            {
                const bool needSeparator = !(out.Size() > 0 && out.Data()[out.Size() - 1] == '\\');

                if ((needSeparator && !Append(out, "\\")) || !Append(out, missing[i - 1]))
                {
                    return PathError::TooLong;
                }
            }

            return PathError::None;
        }

        if (missing.PushBack(LastComponent(prefix)).HasError())
        {
            return PathError::TooLong;
        }

        prefix = ParentPath(prefix);
    }
}

/// Like Resolve, but leaves the last component as written: the folders
/// above it are resolved, the last component itself is not followed.
PathError ResolveParent(const CanonicalPath& lexical, String& out) noexcept
{
    out.Clear();

    // A last component that looks like a short name cannot be expanded
    // without following it, so it is refused like a missing short name.
    if (lexical.lastShortNameComponent == lexical.componentCount)
    {
        return PathError::ShortName;
    }

    CanonicalPath parent;

    if (parent.text.Append(ParentPath(lexical.text.View())).HasError())
    {
        return PathError::TooLong;
    }

    parent.componentCount = lexical.componentCount - 1;
    parent.lastShortNameComponent = lexical.lastShortNameComponent;
    parent.isUnc = lexical.isUnc;

    String resolvedParent;
    const PathError error = Resolve(parent, resolvedParent);

    if (error != PathError::None)
    {
        return error;
    }

    const bool needSeparator =
        !(resolvedParent.Size() > 0 && resolvedParent.Data()[resolvedParent.Size() - 1] == '\\');

    if (!Append(out, resolvedParent.View()) || (needSeparator && !Append(out, "\\")) ||
        !Append(out, LastComponent(lexical.text.View())))
    {
        return PathError::TooLong;
    }

    return PathError::None;
}

/// Ops whose last path component is the link itself, never its target.
bool ActsOnLinkItself(Op op) noexcept
{
    switch (op)
    {
    case Op::FsRmSync:
    case Op::FsRm:
    case Op::FsRenameSync:
    case Op::FsRename:
    case Op::FsLstatSync:
    case Op::FsLstat:
        return true;
    default:
        return false;
    }
}

//------------------------------------------------------------------------------
// Policy queries
//------------------------------------------------------------------------------

bool IsProtected(StringView path) noexcept
{
    if (!g_state.installDir.Empty() && PathCovers(g_state.installDir.View(), path))
    {
        return true;
    }

    if (!g_state.auditPath.Empty() && PathEquals(g_state.auditPath.View(), path))
    {
        return true;
    }

    if (HasComponent(path, ".git"))
    {
        return true;
    }

    const StringView last = LastComponent(path);
    return PathEquals(last, "forge.json") || PathEquals(last, "forge.lock");
}

bool CoveredByTyped(const Vector<Grant>& grants, StringView path) noexcept
{
    for (Size i = 0; i < grants.Size(); ++i)
    {
        if (PathCovers(grants[i].asTyped.View(), path) || PathCovers(grants[i].resolved.View(), path))
        {
            return true;
        }
    }

    return false;
}

bool CoveredByResolved(const Vector<Grant>& grants, StringView path) noexcept
{
    for (Size i = 0; i < grants.Size(); ++i)
    {
        if (PathCovers(grants[i].resolved.View(), path))
        {
            return true;
        }
    }

    return false;
}

//------------------------------------------------------------------------------
// Audit log (JSON lines)
//------------------------------------------------------------------------------

void AppendJsonString(String& out, StringView text) noexcept
{
    out.Append('"').Ignore();

    for (Size i = 0; i < text.Size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text.Data()[i]);

        if (c == '"' || c == '\\')
        {
            out.Append('\\').Ignore();
            out.Append(static_cast<char>(c)).Ignore();
        }
        else if (c < 0x20)
        {
            char escape[8];
            snprintf(escape, sizeof(escape), "\\u%04x", c);
            out.Append(escape).Ignore();
        }
        else
        {
            out.Append(static_cast<char>(c)).Ignore();
        }
    }

    out.Append('"').Ignore();
}

void AppendTimestamp(String& out) noexcept
{
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char buffer[40];
    snprintf(buffer, sizeof(buffer), "\"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\"", now.wYear,
             now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    out.Append(buffer).Ignore();
}

bool WriteAuditLine(const String& line) noexcept
{
    if (g_state.audit == INVALID_HANDLE_VALUE)
    {
        return true;
    }

    if (g_state.auditBroken)
    {
        return false;
    }

    DWORD written = 0;

    if (!WriteFile(g_state.audit, line.Data(), static_cast<DWORD>(line.Size()), &written, nullptr) ||
        written != line.Size())
    {
        g_state.auditBroken = true;
        return false;
    }

    return true;
}

bool WriteSuppressedSummary(Op op, AuditBucket& bucket) noexcept
{
    if (bucket.suppressedAllow == 0 && bucket.suppressedDeny == 0)
    {
        return true;
    }

    String line;
    char counts[96];
    snprintf(counts, sizeof(counts), ",\"suppressed\":{\"allow\":%llu,\"deny\":%llu}}\n",
             bucket.suppressedAllow, bucket.suppressedDeny);
    line.Append("{\"time\":").Ignore();
    AppendTimestamp(line);
    line.Append(",\"op\":").Ignore();
    AppendJsonString(line, GetOpInfo(op).name);
    line.Append(counts).Ignore();

    if (!WriteAuditLine(line))
    {
        return false;
    }

    bucket.suppressedAllow = 0;
    bucket.suppressedDeny = 0;
    return true;
}

const char* DenialReason(Denial::Kind kind) noexcept
{
    switch (kind)
    {
    case Denial::Kind::None: return "allowed";
    case Denial::Kind::InvalidPath: return "invalid-path";
    case Denial::Kind::NotGranted: return "not-granted";
    case Denial::Kind::Protected: return "protected";
    case Denial::Kind::AuditFailed: return "audit-failed";
    case Denial::Kind::Internal: return "internal-error";
    case Denial::Kind::Link: return "link";
    case Denial::Kind::HardLink: return "hard-link";
    case Denial::Kind::Changed: return "changed-after-check";
    }

    return "unknown";
}

/// Records one decision. Returns false if the record could not be written;
/// the caller must then refuse the operation (R7).
bool Audit(Op op, const char* permission, StringView target, Denial::Kind kind) noexcept
{
    if (g_state.audit == INVALID_HANDLE_VALUE)
    {
        return true;
    }

    forge::core::LockGuard<forge::core::Mutex> lock(g_auditMutex);

    if (g_state.auditBroken)
    {
        return false;
    }

    AuditBucket& bucket = g_state.buckets[static_cast<size_t>(op)];
    const ULONGLONG now = GetTickCount64();

    if (bucket.lastMs != 0)
    {
        bucket.tokens += static_cast<double>(now - bucket.lastMs) / 1000.0 * kAuditPerSecond;

        if (bucket.tokens > kAuditBurst)
        {
            bucket.tokens = kAuditBurst;
        }
    }

    bucket.lastMs = now;

    if (bucket.tokens < 1)
    {
        if (kind == Denial::Kind::None)
        {
            ++bucket.suppressedAllow;
        }
        else
        {
            ++bucket.suppressedDeny;
        }

        return true;
    }

    bucket.tokens -= 1;

    if (!WriteSuppressedSummary(op, bucket))
    {
        return false;
    }

    String line;
    line.Append("{\"time\":").Ignore();
    AppendTimestamp(line);
    line.Append(",\"op\":").Ignore();
    AppendJsonString(line, GetOpInfo(op).name);
    line.Append(",\"permission\":").Ignore();
    AppendJsonString(line, permission);
    line.Append(",\"target\":").Ignore();
    AppendJsonString(line, target);
    line.Append(",\"decision\":").Ignore();
    line.Append(kind == Denial::Kind::None ? "\"allow\"" : "\"deny\"").Ignore();

    if (kind != Denial::Kind::None)
    {
        line.Append(",\"reason\":").Ignore();
        AppendJsonString(line, DenialReason(kind));
    }

    if (line.Append("}\n").HasError())
    {
        g_state.auditBroken = true;
        return false;
    }

    return WriteAuditLine(line);
}

/// Records a refusal and fills `denial`. Always returns the error to throw.
template <typename T>
Result<T> Refuse(Op op, Denial& denial, Denial::Kind kind, StringView target,
                 PathError pathError = PathError::None) noexcept
{
    denial.kind = kind;
    denial.pathError = pathError;
    denial.target.Clear();
    denial.target.Append(target).Ignore();
    // A failed write cannot make a refusal worse.
    Audit(op, AuditPermissionName(op, denial.access), target, kind);

    const ErrorCode code =
        kind == Denial::Kind::InvalidPath ? ErrorCode::InvalidArgument : ErrorCode::PermissionDenied;
    return Result<T>(Failure{ Error(code) });
}

//------------------------------------------------------------------------------
// Startup
//------------------------------------------------------------------------------

/// Canonicalizes and resolves a path given on the command line (or found by
/// asking Windows). `what` names the option for error messages.
bool CanonicalizeOptionPath(StringView input, const char* what, Grant& grant,
                            String& message) noexcept
{
    CanonicalPath lexical;
    PathError error = LexicalCanonicalize(input, g_state.cwd.View(), lexical);

    if (error == PathError::None)
    {
        error = Resolve(lexical, grant.resolved);
    }

    if (error != PathError::None)
    {
        Append(message, what);
        Append(message, ": \"");
        Append(message, input);
        Append(message, "\" is not a valid path: ");
        Append(message, DescribePathError(error));
        Append(message, ".");
        return false;
    }

    grant.asTyped.Swap(lexical.text);
    return true;
}

/// Asks Windows for a folder and stores it canonical and resolved.
bool SetSystemPath(const String& override, String& target, const wchar_t* (*query)(Vector<wchar_t>&),
                   const char* what, String& message) noexcept
{
    Grant grant;

    if (!override.Empty())
    {
        if (!CanonicalizeOptionPath(override.View(), what, grant, message))
        {
            return false;
        }

        target.Swap(grant.resolved);
        return true;
    }

    Vector<wchar_t> buffer;
    const wchar_t* text = query(buffer);

    if (text == nullptr)
    {
        return true;  // unknown; the rule that needs it is skipped
    }

    String utf8;
    Size length = 0;

    while (text[length] != L'\0')
    {
        ++length;
    }

    if (forge::core::platform::AppendUtf16AsUtf8(text, length, utf8).HasError() ||
        !CanonicalizeOptionPath(utf8.View(), what, grant, message))
    {
        if (message.Empty())
        {
            Append(message, "out of memory while reading ");
            Append(message, what);
        }

        return false;
    }

    target.Swap(grant.resolved);
    return true;
}

const wchar_t* QueryCurrentDirectory(Vector<wchar_t>& buffer) noexcept
{
    const DWORD needed = GetCurrentDirectoryW(0, nullptr);

    if (needed == 0 || buffer.Reserve(needed).HasError())
    {
        return nullptr;
    }

    for (DWORD i = 0; i < needed; ++i)
    {
        buffer.PushBack(L'\0').Ignore();
    }

    return GetCurrentDirectoryW(needed, buffer.Data()) != 0 ? buffer.Data() : nullptr;
}

const wchar_t* QueryInstallDirectory(Vector<wchar_t>& buffer) noexcept
{
    DWORD capacity = 512;

    for (int attempt = 0; attempt < 6; ++attempt, capacity *= 2)
    {
        buffer.Clear();

        if (buffer.Reserve(capacity).HasError())
        {
            return nullptr;
        }

        for (DWORD i = 0; i < capacity; ++i)
        {
            buffer.PushBack(L'\0').Ignore();
        }

        const DWORD length = GetModuleFileNameW(nullptr, buffer.Data(), capacity);

        if (length == 0)
        {
            return nullptr;
        }

        if (length < capacity)
        {
            // Cut the file name off: keep the folder.
            DWORD end = length;

            while (end > 0 && buffer[end - 1] != L'\\' && buffer[end - 1] != L'/')
            {
                --end;
            }

            if (end == 0)
            {
                return nullptr;
            }

            buffer[end - 1] = L'\0';
            return buffer.Data();
        }
    }

    return nullptr;
}

const wchar_t* QueryHomeDirectory(Vector<wchar_t>& buffer) noexcept
{
    String value;
    Result<bool> found = forge::core::platform::GetEnv("USERPROFILE", value);

    if (found.HasError() || !found.Value() || value.Empty())
    {
        return nullptr;
    }

    Result<Vector<wchar_t>> wide = forge::core::platform::Utf8ToUtf16(value.View());

    if (wide.HasError())
    {
        return nullptr;
    }

    buffer = std::move(wide.Value());
    return buffer.Data();
}

bool AddGrants(const Vector<String>& paths, const char* option, Vector<Grant>& grants,
               String& message) noexcept
{
    for (Size i = 0; i < paths.Size(); ++i)
    {
        Grant grant;

        if (!CanonicalizeOptionPath(paths[i].View(), option, grant, message))
        {
            return false;
        }

        const StringView resolved = grant.resolved.View();
        const char* refusal = nullptr;

        if (IsRootPath(resolved) || IsRootPath(grant.asTyped.View()))
        {
            refusal = "a grant may not cover a whole drive or network share";
        }
        else if (!g_state.homeDir.Empty() && PathCovers(resolved, g_state.homeDir.View()))
        {
            refusal = "a grant may not cover your whole home folder";
        }
        else if (IsProtected(resolved))
        {
            refusal = "it is a protected path (a .git folder, forge.json, forge.lock, the audit "
                      "log or Forge's install folder)";
        }

        if (refusal != nullptr)
        {
            Append(message, option);
            Append(message, "=");
            Append(message, paths[i].View());
            Append(message, " is refused: ");
            Append(message, refusal);
            Append(message, ". Grant a folder inside it instead.");
            return false;
        }

        if (grants.PushBack(std::move(grant)).HasError())
        {
            Append(message, "out of memory while reading the permissions");
            return false;
        }
    }

    return true;
}

void AppendGrantList(String& line, const Vector<Grant>& grants) noexcept
{
    line.Append('[').Ignore();

    for (Size i = 0; i < grants.Size(); ++i)
    {
        if (i > 0)
        {
            line.Append(',').Ignore();
        }

        AppendJsonString(line, grants[i].resolved.View());
    }

    line.Append(']').Ignore();
}

bool OpenAuditLog(const String& path, String& message) noexcept
{
    Grant grant;

    if (!CanonicalizeOptionPath(path.View(), "--audit-log", grant, message))
    {
        return false;
    }

    Vector<wchar_t> wide;

    if (!ToNtStyleWide(grant.resolved.View(), wide))
    {
        Append(message, "out of memory while opening the audit log");
        return false;
    }

    // Append-only; others may read it while Forge runs, nobody may write.
    g_state.audit = CreateFileW(wide.Data(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);

    if (g_state.audit == INVALID_HANDLE_VALUE)
    {
        char code[48];
        snprintf(code, sizeof(code), " (Windows error %lu)", GetLastError());
        Append(message, "--audit-log: cannot open \"");
        Append(message, grant.resolved.View());
        Append(message, "\" for writing");
        Append(message, code);
        return false;
    }

    g_state.auditPath.Swap(grant.resolved);
    return true;
}

} // namespace

//------------------------------------------------------------------------------
// The checker. A class only so CheckedPath can name it as its sole friend.
//------------------------------------------------------------------------------

class Checker
{
public:
    static Result<CheckedPath> Check(Op op, Access access, StringView input, Denial& denial) noexcept
    {
        denial = Denial();
        denial.access = access;

        if (!g_state.initialized || !OpAllows(GetOpInfo(op).permission, access))
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::Internal, input);
        }

        CanonicalPath lexical;
        const PathError lexicalError = LexicalCanonicalize(input, g_state.cwd.View(), lexical);

        if (lexicalError != PathError::None)
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::InvalidPath, input, lexicalError);
        }

        // Decide on the path as written first: nothing is opened (and no
        // network share is contacted) for a path no grant could cover.
        const Vector<Grant>& grants =
            access == Access::Write ? g_state.writeGrants : g_state.readGrants;

        if (IsProtected(lexical.text.View()))
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::Protected, lexical.text.View());
        }

        if (!CoveredByTyped(grants, lexical.text.View()))
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::NotGranted, lexical.text.View());
        }

        // Then on where it really leads. Ops that act on a link itself
        // (rm, rename, lstat) resolve only the folders above the last
        // component (Phase 8.3): removing a junction inside a granted folder
        // removes the link, so the link's own location is what is checked,
        // not its target.
        String resolved;
        const PathError resolveError = ActsOnLinkItself(op) && lexical.componentCount > 0
                                           ? ResolveParent(lexical, resolved)
                                           : Resolve(lexical, resolved);

        if (resolveError == PathError::ShortName)
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::InvalidPath, lexical.text.View(),
                                       resolveError);
        }

        if (resolveError != PathError::None)
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::Internal, lexical.text.View());
        }

        if (IsProtected(resolved.View()))
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::Protected, resolved.View());
        }

        if (!CoveredByResolved(grants, resolved.View()))
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::NotGranted, resolved.View());
        }

        if (!Audit(op, AuditPermissionName(op, access), resolved.View(), Denial::Kind::None))
        {
            denial.kind = Denial::Kind::AuditFailed;
            denial.target.Append(resolved.View()).Ignore();
            return Result<CheckedPath>(Failure{ Error(ErrorCode::PermissionDenied) });
        }

        CheckedPath checked;
        Result<forge::core::Path> path = forge::core::Path::Create(resolved.View());

        if (path.HasError())
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::Internal, resolved.View());
        }

        checked.path_ = std::move(path.Value());
        checked.text_.Swap(resolved);
        checked.op_ = op;
        checked.access_ = access;
        return Result<CheckedPath>(std::move(checked));
    }
};

bool Initialize(
    const PolicyOptions& options,
    String& message) noexcept
{
    message.Clear();

    if (g_state.initialized)
    {
        Append(message, "Kavach was already initialized");
        return false;
    }

    // The working directory first: every relative path is resolved
    // against it, including the other system folders' overrides. Forge
    // never changes it later.
    if (!SetSystemPath(options.cwd, g_state.cwd, QueryCurrentDirectory, "the current folder",
                       message) ||
        !SetSystemPath(options.installDir, g_state.installDir, QueryInstallDirectory,
                       "Forge's install folder", message) ||
        !SetSystemPath(options.homeDir, g_state.homeDir, QueryHomeDirectory, "the home folder",
                       message))
    {
        return false;
    }

    if (g_state.cwd.Empty())
    {
        Append(message, "cannot determine the current folder");
        return false;
    }

    // The audit log is protected before any grant is checked against it.
    if (!options.auditLogPath.Empty() && !OpenAuditLog(options.auditLogPath, message))
    {
        return false;
    }

    if (!AddGrants(options.readPaths, "--allow-read", g_state.readGrants, message) ||
        !AddGrants(options.writePaths, "--allow-write", g_state.writeGrants, message))
    {
        return false;
    }

    for (Size i = 0; i < options.envNames.Size(); ++i)
    {
        if (!forge::core::platform::IsValidEnvName(options.envNames[i].View()))
        {
            Append(message, "--allow-env: \"");
            Append(message, options.envNames[i].View());
            Append(message, "\" is not a valid variable name");
            return false;
        }

        String copy;

        if (copy.Append(options.envNames[i].View()).HasError() ||
            g_state.envNames.PushBack(std::move(copy)).HasError())
        {
            Append(message, "out of memory while reading the permissions");
            return false;
        }
    }

    g_state.allEnv = options.allEnv;

    if (g_state.audit != INVALID_HANDLE_VALUE)
    {
        String line;
        char pid[32];
        snprintf(pid, sizeof(pid), "%lu", GetCurrentProcessId());
        line.Append("{\"time\":").Ignore();
        AppendTimestamp(line);
        line.Append(",\"event\":\"start\",\"pid\":").Ignore();
        line.Append(pid).Ignore();
        line.Append(",\"read\":").Ignore();
        AppendGrantList(line, g_state.readGrants);
        line.Append(",\"write\":").Ignore();
        AppendGrantList(line, g_state.writeGrants);
        line.Append(",\"env\":").Ignore();

        if (g_state.allEnv)
        {
            line.Append("\"*\"").Ignore();
        }
        else
        {
            line.Append('[').Ignore();

            for (Size i = 0; i < g_state.envNames.Size(); ++i)
            {
                if (i > 0)
                {
                    line.Append(',').Ignore();
                }

                AppendJsonString(line, g_state.envNames[i].View());
            }

            line.Append(']').Ignore();
        }

        line.Append("}\n").Ignore();

        if (!WriteAuditLine(line))
        {
            Append(message, "--audit-log: the first record could not be written");
            return false;
        }
    }

    g_state.initialized = true;
    return true;
}

void Shutdown() noexcept
{
    if (g_state.audit != INVALID_HANDLE_VALUE)
    {
        for (size_t i = 0; i < kOpCount; ++i)
        {
            WriteSuppressedSummary(static_cast<Op>(i), g_state.buckets[i]);
        }

        String line;
        line.Append("{\"time\":").Ignore();
        AppendTimestamp(line);
        line.Append(",\"event\":\"stop\"}\n").Ignore();
        WriteAuditLine(line);
        CloseHandle(g_state.audit);
        g_state.audit = INVALID_HANDLE_VALUE;
    }
}

Result<CheckedPath> CheckPath(
    Op op,
    StringView path,
    Denial& denial) noexcept
{
    const Access access =
        GetOpInfo(op).permission == Permission::Write ? Access::Write : Access::Read;
    return Checker::Check(op, access, path, denial);
}

Result<CheckedPath> CheckPath(
    Op op,
    Access access,
    StringView path,
    Denial& denial) noexcept
{
    return Checker::Check(op, access, path, denial);
}

bool VerifyOpenedPath(
    const CheckedPath& checked,
    StringView finalPath,
    Denial& denial) noexcept
{
    denial = Denial();
    denial.access = checked.GetAccess();

    if (IsProtected(finalPath))
    {
        RecordLateDenial(checked, denial, Denial::Kind::Protected, finalPath);
        return false;
    }

    const Vector<Grant>& grants =
        checked.GetAccess() == Access::Write ? g_state.writeGrants : g_state.readGrants;

    if (!CoveredByResolved(grants, finalPath))
    {
        RecordLateDenial(checked, denial, Denial::Kind::Changed, finalPath);
        return false;
    }

    return true;
}

void RecordLateDenial(
    const CheckedPath& checked,
    Denial& denial,
    Denial::Kind kind,
    StringView target) noexcept
{
    denial.kind = kind;
    denial.access = checked.GetAccess();
    denial.target.Clear();
    denial.target.Append(target).Ignore();
    Audit(checked.GetOp(), AuditPermissionName(checked.GetOp(), checked.GetAccess()), target, kind);
}

Result<void> CheckEnv(
    Op op,
    StringView name,
    Denial& denial) noexcept
{
    denial = Denial();

    if (!g_state.initialized || GetOpInfo(op).permission != Permission::Env)
    {
        return Refuse<void>(op, denial, Denial::Kind::Internal, name);
    }

    bool granted = g_state.allEnv;

    for (Size i = 0; i < g_state.envNames.Size() && !granted; ++i)
    {
        granted = PathEquals(g_state.envNames[i].View(), name);  // ASCII case-insensitive
    }

    if (!granted)
    {
        return Refuse<void>(op, denial, Denial::Kind::NotGranted, name);
    }

    if (!Audit(op, PermissionName(Permission::Env), name, Denial::Kind::None))
    {
        denial.kind = Denial::Kind::AuditFailed;
        denial.target.Append(name).Ignore();
        return Result<void>(Failure{ Error(ErrorCode::PermissionDenied) });
    }

    return Result<void>();
}

Result<void> CheckEnvAll(
    Op op,
    Denial& denial) noexcept
{
    denial = Denial();

    if (!g_state.initialized || GetOpInfo(op).permission != Permission::EnvAll)
    {
        return Refuse<void>(op, denial, Denial::Kind::Internal, "*");
    }

    if (!g_state.allEnv)
    {
        return Refuse<void>(op, denial, Denial::Kind::NotGranted, "*");
    }

    if (!Audit(op, PermissionName(Permission::EnvAll), "*", Denial::Kind::None))
    {
        denial.kind = Denial::Kind::AuditFailed;
        return Result<void>(Failure{ Error(ErrorCode::PermissionDenied) });
    }

    return Result<void>();
}

void FormatDenial(
    Op op,
    const Denial& denial,
    String& message) noexcept
{
    const OpInfo& info = GetOpInfo(op);
    const StringView target = denial.target.View();

    switch (denial.kind)
    {
    case Denial::Kind::InvalidPath:
        Append(message, info.name);
        Append(message, ": the path \"");
        Append(message, target);
        Append(message, "\" is not accepted: ");
        Append(message, DescribePathError(denial.pathError));
        Append(message, ".");
        return;

    case Denial::Kind::Protected:
        Append(message, "Permission denied: ");
        Append(message, info.name);
        Append(message, " cannot use ");
        Append(message, target);
        Append(message, ". It is a protected path (a .git folder, forge.json, forge.lock, the "
                        "audit log or Forge's install folder); no permission can grant access to it.");
        return;

    case Denial::Kind::NotGranted:
        Append(message, "Permission denied: ");
        Append(message, info.name);

        if (info.permission == Permission::Read || info.permission == Permission::Write ||
            info.permission == Permission::ReadWrite)
        {
            const bool read = denial.access == Access::Read;
            Append(message, read ? " needs read access to " : " needs write access to ");
            Append(message, target);
            Append(message, ".\nTo allow it, run Forge with ");
            Append(message, read ? "--allow-read=" : "--allow-write=");
            Append(message, target);
            Append(message, " (or a folder that contains it).");
        }
        else if (info.permission == Permission::Env)
        {
            Append(message, " needs access to the environment variable ");
            Append(message, target);
            Append(message, ".\nTo allow it, run Forge with --allow-env=");
            Append(message, target);
            Append(message, ".");
        }
        else
        {
            Append(message, " needs access to every environment variable.\nTo allow it, run Forge "
                            "with --allow-env (with no list).");
        }
        return;

    case Denial::Kind::AuditFailed:
        Append(message, "Permission denied: ");
        Append(message, info.name);
        Append(message, " was refused because the audit log could not be written (Kavach fails "
                        "closed).");
        return;

    case Denial::Kind::Link:
        Append(message, "Permission denied: ");
        Append(message, info.name);
        Append(message, " will not write through ");
        Append(message, target);
        Append(message, ", which is a symbolic link or junction. Write to the real location "
                        "instead (Forge never writes or deletes through links).");
        return;

    case Denial::Kind::HardLink:
        Append(message, "Permission denied: ");
        Append(message, info.name);
        Append(message, " will not change ");
        Append(message, target);
        Append(message, ", which has more than one hard link: the same file may also be reachable "
                        "from outside the granted folders.");
        return;

    case Denial::Kind::Changed:
        Append(message, "Permission denied: ");
        Append(message, info.name);
        Append(message, " found that the path led to ");
        Append(message, target);
        Append(message, " when it was opened, which no grant covers. A link or folder may have "
                        "been changed after the check.");
        return;

    case Denial::Kind::Internal:
    case Denial::Kind::None:
        Append(message, "Permission denied: ");
        Append(message, info.name);
        Append(message, " was refused because Kavach could not check it (out of memory, or an "
                        "unexpected answer from Windows).");
        return;
    }
}

void DescribePolicy(
    String& out) noexcept
{
    for (Size i = 0; i < g_state.readGrants.Size(); ++i)
    {
        Append(out, "read  ");
        Append(out, g_state.readGrants[i].resolved.View());
        Append(out, "\n");
    }

    for (Size i = 0; i < g_state.writeGrants.Size(); ++i)
    {
        Append(out, "write ");
        Append(out, g_state.writeGrants[i].resolved.View());
        Append(out, "\n");
    }

    if (g_state.allEnv)
    {
        Append(out, "env   (every variable)\n");
    }

    for (Size i = 0; i < g_state.envNames.Size(); ++i)
    {
        Append(out, "env   ");
        Append(out, g_state.envNames[i].View());
        Append(out, "\n");
    }
}

} // namespace forge::kavach
