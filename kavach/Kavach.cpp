#include "kavach/Kavach.h"

#include <stdio.h>

#include <utility>

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "forge-core/LockGuard.h"
#include "forge-core/Mutex.h"
#include "forge-core/memory/MakeUnique.h"
#include "forge-core/memory/UniquePtr.h"
#include "forge-core/platform/Environment.h"
#include "forge-core/platform/Unicode.h"

namespace forge::kavach
{

/// A grant: one path, canonical as written and resolved.
struct Grant
{
    forge::core::String asTyped;   // canonical, before resolving links (lexical)
    forge::core::String resolved;  // canonical, links and short names resolved
};

/// Phase 9.4: see Kavach.h.
class PackagePolicy
{
public:
    forge::core::String name;
    forge::core::Vector<Grant> declaredRead;
    forge::core::Vector<Grant> declaredWrite;
    forge::core::Vector<forge::core::String> declaredEnv;
    forge::core::Vector<Grant> grantedRead;
    forge::core::Vector<Grant> grantedWrite;
    forge::core::Vector<forge::core::String> grantedEnv;
    bool noPermission = false;  // NoPermissionPolicy: refuses everything
};

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
    Vector<Grant> moduleRoots;  // Phase 9.1 (D23): only Op::ModuleImport uses these
    Vector<String> envNames;
    bool allEnv = false;

    String auditPath;
    HANDLE audit = INVALID_HANDLE_VALUE;
    bool auditBroken = false;
    AuditBucket buckets[kOpCount];

    /// Phase 9.4: added on the main thread as packages load; never removed
    /// before Shutdown, so the pointers stay valid for worker threads.
    Vector<forge::core::memory::UniquePtr<PackagePolicy>> policies;
};

State g_state;
PackagePolicy g_noPermission;  // NoPermissionPolicy(); noPermission set in Initialize

// Phase 8.3: filesystem work runs on worker threads too, and they may need
// to record a late refusal. Every audit write holds this lock. The policy
// itself never changes after Initialize(), so reading it needs no lock.
forge::core::Mutex g_auditMutex;

/// The grants that decide `op` with `access` (Phase 9.1: module loads use
/// the module roots, never the read grants).
const Vector<Grant>& GrantsFor(Op op, Access access) noexcept
{
    if (GetOpInfo(op).permission == Permission::Module)
    {
        return g_state.moduleRoots;
    }

    return access == Access::Write ? g_state.writeGrants : g_state.readGrants;
}

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

/// Phase 9.4: the audit reason of a package's refusal.
const char* LayerReason(Denial::Layer layer) noexcept
{
    switch (layer)
    {
    case Denial::Layer::NotDeclared: return "not-declared";
    case Denial::Layer::NotGranted: return "not-granted-by-program";
    case Denial::Layer::CommandLine: return "not-allowed-by-command-line";
    case Denial::Layer::None: break;
    }

    return "not-granted";
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

/// The audit "package" of a policy; empty for the program.
StringView AuditPackageName(const PackagePolicy* policy) noexcept
{
    if (policy == nullptr)
    {
        return StringView();
    }

    return policy->noPermission ? StringView("(a package without permissions)") : policy->name.View();
}

/// Records one decision. Returns false if the record could not be written;
/// the caller must then refuse the operation (R7). Phase 9.4: `policy`
/// names the package (none for the program) and `layer` refines the reason.
bool Audit(Op op, const char* permission, StringView target, Denial::Kind kind,
           const PackagePolicy* policy = nullptr,
           Denial::Layer layer = Denial::Layer::None) noexcept
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

    if (policy != nullptr)
    {
        line.Append(",\"package\":").Ignore();
        AppendJsonString(line, AuditPackageName(policy));
    }

    line.Append(",\"permission\":").Ignore();
    AppendJsonString(line, permission);
    line.Append(",\"target\":").Ignore();
    AppendJsonString(line, target);
    line.Append(",\"decision\":").Ignore();
    line.Append(kind == Denial::Kind::None ? "\"allow\"" : "\"deny\"").Ignore();

    if (kind != Denial::Kind::None)
    {
        line.Append(",\"reason\":").Ignore();
        AppendJsonString(line, kind == Denial::Kind::NotGranted && layer != Denial::Layer::None
                                   ? LayerReason(layer)
                                   : DenialReason(kind));
    }

    if (line.Append("}\n").HasError())
    {
        g_state.auditBroken = true;
        return false;
    }

    return WriteAuditLine(line);
}

/// Records a refusal and fills `denial`. Always returns the error to throw.
/// Phase 9.4: `policy` is the package's (null for the program); a
/// NotGranted refusal of a package carries `denial.layer`, set by the caller.
template <typename T>
Result<T> Refuse(Op op, Denial& denial, Denial::Kind kind, StringView target,
                 PathError pathError = PathError::None,
                 const PackagePolicy* policy = nullptr) noexcept
{
    denial.kind = kind;
    denial.pathError = pathError;
    denial.target.Clear();
    denial.target.Append(target).Ignore();

    if (kind != Denial::Kind::NotGranted)
    {
        denial.layer = Denial::Layer::None;
    }

    // A failed write cannot make a refusal worse.
    Audit(op, AuditPermissionName(op, denial.access), target, kind, policy, denial.layer);

    const ErrorCode code =
        kind == Denial::Kind::InvalidPath ? ErrorCode::InvalidArgument : ErrorCode::PermissionDenied;
    return Result<T>(Failure{ Error(code) });
}

//------------------------------------------------------------------------------
// Startup
//------------------------------------------------------------------------------

/// Canonicalizes and resolves a path given on the command line (or found by
/// asking Windows), relative to `base` (the current folder unless given).
/// `what` names the option for error messages.
bool CanonicalizeOptionPath(StringView input, StringView what, Grant& grant, String& message,
                            StringView base = StringView()) noexcept
{
    CanonicalPath lexical;
    PathError error = LexicalCanonicalize(input, base.Empty() ? g_state.cwd.View() : base, lexical);

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

/// Adds grants for `paths` (relative to `base`, the current folder unless
/// given). `option` names their source in messages ("--allow-read", or
/// Phase 9.4 a forge.json list).
bool AddGrants(const Vector<String>& paths, StringView option, Vector<Grant>& grants,
               String& message, StringView base = StringView()) noexcept
{
    for (Size i = 0; i < paths.Size(); ++i)
    {
        Grant grant;

        if (!CanonicalizeOptionPath(paths[i].View(), option, grant, message, base))
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
            Append(message, base.Empty() ? "=" : " \"");
            Append(message, paths[i].View());
            Append(message, base.Empty() ? " is refused: " : "\" is refused: ");
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

namespace
{

/// Phase 9.4: true if `path` is allowed for `op` with `access` under
/// `policy` (null: the program, the command line alone). For a package all
/// three layers must agree (D26); `layer` receives the first that refuses.
/// `resolvedOnly`: match against the resolved grants only (the second
/// check, and the re-check on an opened handle).
bool Covered(const PackagePolicy* policy, Op op, Access access, StringView path, bool resolvedOnly,
             Denial::Layer& layer) noexcept
{
    layer = Denial::Layer::None;

    auto covered = [path, resolvedOnly](const Vector<Grant>& grants)
    {
        return resolvedOnly ? CoveredByResolved(grants, path) : CoveredByTyped(grants, path);
    };

    const Vector<Grant>& commandLine = GrantsFor(op, access);

    if (policy == nullptr || GetOpInfo(op).permission == Permission::Module)
    {
        return covered(commandLine);
    }

    const bool write = access == Access::Write;

    if (policy->noPermission || !covered(write ? policy->declaredWrite : policy->declaredRead))
    {
        layer = Denial::Layer::NotDeclared;
        return false;
    }

    if (!covered(write ? policy->grantedWrite : policy->grantedRead))
    {
        layer = Denial::Layer::NotGranted;
        return false;
    }

    if (!covered(commandLine))
    {
        layer = Denial::Layer::CommandLine;
        return false;
    }

    return true;
}

bool HasName(const Vector<String>& names, StringView name) noexcept
{
    for (Size i = 0; i < names.Size(); ++i)
    {
        if (PathEquals(names[i].View(), name))  // ASCII case-insensitive, as Windows
        {
            return true;
        }
    }

    return false;
}

/// True if the two lists share any path (one covers the other).
bool Overlaps(const Vector<Grant>& a, const Vector<Grant>& b) noexcept
{
    for (Size i = 0; i < a.Size(); ++i)
    {
        for (Size j = 0; j < b.Size(); ++j)
        {
            if (PathCovers(a[i].resolved.View(), b[j].resolved.View()) ||
                PathCovers(b[j].resolved.View(), a[i].resolved.View()))
            {
                return true;
            }
        }
    }

    return false;
}

} // namespace

//------------------------------------------------------------------------------
// The checker. A class only so CheckedPath can name it as its sole friend.
//------------------------------------------------------------------------------

class Checker
{
public:
    static Result<CheckedPath> Check(const PackagePolicy* policy, Op op, Access access,
                                     StringView input, Denial& denial) noexcept
    {
        denial = Denial();
        denial.access = access;

        // Phase 9.4: module loading is decided by the module roots alone.
        if (GetOpInfo(op).permission == Permission::Module)
        {
            policy = nullptr;
        }

        if (policy != nullptr)
        {
            denial.package.Append(AuditPackageName(policy)).Ignore();
        }

        auto refuse = [op, &denial, policy](Denial::Kind kind, StringView target,
                                            PathError pathError = PathError::None)
        {
            return Refuse<CheckedPath>(op, denial, kind, target, pathError, policy);
        };

        if (!g_state.initialized || !OpAllows(GetOpInfo(op).permission, access))
        {
            return refuse(Denial::Kind::Internal, input);
        }

        CanonicalPath lexical;
        const PathError lexicalError = LexicalCanonicalize(input, g_state.cwd.View(), lexical);

        if (lexicalError != PathError::None)
        {
            return refuse(Denial::Kind::InvalidPath, input, lexicalError);
        }

        // Decide on the path as written first: nothing is opened (and no
        // network share is contacted) for a path no grant could cover.
        if (IsProtected(lexical.text.View()))
        {
            return refuse(Denial::Kind::Protected, lexical.text.View());
        }

        if (!Covered(policy, op, access, lexical.text.View(), false, denial.layer))
        {
            return refuse(Denial::Kind::NotGranted, lexical.text.View());
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
            return refuse(Denial::Kind::InvalidPath, lexical.text.View(), resolveError);
        }

        if (resolveError != PathError::None)
        {
            return refuse(Denial::Kind::Internal, lexical.text.View());
        }

        if (IsProtected(resolved.View()))
        {
            return refuse(Denial::Kind::Protected, resolved.View());
        }

        if (!Covered(policy, op, access, resolved.View(), true, denial.layer))
        {
            return refuse(Denial::Kind::NotGranted, resolved.View());
        }

        if (!Audit(op, AuditPermissionName(op, access), resolved.View(), Denial::Kind::None, policy))
        {
            denial.kind = Denial::Kind::AuditFailed;
            denial.target.Append(resolved.View()).Ignore();
            return Result<CheckedPath>(Failure{ Error(ErrorCode::PermissionDenied) });
        }

        CheckedPath checked;
        Result<forge::core::Path> path = forge::core::Path::Create(resolved.View());

        if (path.HasError())
        {
            return refuse(Denial::Kind::Internal, resolved.View());
        }

        checked.path_ = std::move(path.Value());
        checked.text_.Swap(resolved);
        checked.op_ = op;
        checked.access_ = access;
        checked.policy_ = policy;
        return Result<CheckedPath>(std::move(checked));
    }

    /// Phase 9.2: see CheckManifest in Kavach.h.
    static Result<CheckedPath> Manifest(StringView packageRoot, Denial& denial) noexcept
    {
        const Op op = Op::ModuleImport;
        denial = Denial();
        denial.access = Access::Read;

        bool isRoot = false;

        for (Size i = 0; i < g_state.moduleRoots.Size() && !isRoot; ++i)
        {
            isRoot = PathEquals(g_state.moduleRoots[i].resolved.View(), packageRoot);
        }

        String text;

        if (!Append(text, packageRoot) || !Append(text, "\\") || !Append(text, "forge.json"))
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::Internal, packageRoot);
        }

        if (!g_state.initialized || !isRoot)
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::NotGranted, text.View());
        }

        CanonicalPath lexical;
        const PathError lexicalError = LexicalCanonicalize(text.View(), g_state.cwd.View(), lexical);

        if (lexicalError != PathError::None)
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::InvalidPath, text.View(), lexicalError);
        }

        String resolved;

        if (Resolve(lexical, resolved) != PathError::None)
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::Internal, lexical.text.View());
        }

        // forge.json itself must not be a link to somewhere else.
        if (!PathEquals(resolved.View(), lexical.text.View()))
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::Link, lexical.text.View());
        }

        if (!Audit(op, AuditPermissionName(op, Access::Read), resolved.View(), Denial::Kind::None))
        {
            denial.kind = Denial::Kind::AuditFailed;
            denial.target.Append(resolved.View()).Ignore();
            return Result<CheckedPath>(Failure{ Error(ErrorCode::PermissionDenied) });
        }

        Result<forge::core::Path> path = forge::core::Path::Create(resolved.View());

        if (path.HasError())
        {
            return Refuse<CheckedPath>(op, denial, Denial::Kind::Internal, resolved.View());
        }

        CheckedPath checked;
        checked.path_ = std::move(path.Value());
        checked.text_.Swap(resolved);
        checked.op_ = op;
        checked.access_ = Access::Read;
        checked.manifest_ = true;
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
    g_noPermission.noPermission = true;  // Phase 9.4

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

bool AddModuleRoot(
    StringView path,
    String& message) noexcept
{
    if (!g_state.initialized)
    {
        Append(message, "internal error: module root added before Kavach was initialized");
        return false;
    }

    Grant root;

    if (!CanonicalizeOptionPath(path, "the program's folder", root, message))
    {
        return false;
    }

    const StringView resolved = root.resolved.View();
    const char* refusal = nullptr;

    if (IsRootPath(resolved) || IsRootPath(root.asTyped.View()))
    {
        refusal = "it is a whole drive or network share";
    }
    else if (!g_state.homeDir.Empty() && PathCovers(resolved, g_state.homeDir.View()))
    {
        refusal = "it contains your whole home folder";
    }
    else if (IsProtected(resolved))
    {
        refusal = "it is a protected path";
    }

    if (refusal != nullptr)
    {
        Append(message, "Forge will not load modules from ");
        Append(message, resolved);
        Append(message, ": ");
        Append(message, refusal);
        Append(message, ". Move the program into a folder of its own.");
        return false;
    }

    if (g_state.moduleRoots.PushBack(std::move(root)).HasError())
    {
        Append(message, "out of memory while setting up module loading");
        return false;
    }

    return true;
}

namespace
{

/// GetFileAttributesW on a canonical path; INVALID_FILE_ATTRIBUTES if it
/// does not exist or cannot be asked about.
DWORD AttributesOf(StringView canonical) noexcept
{
    Vector<wchar_t> wide;

    if (!ToNtStyleWide(canonical, wide))
    {
        return INVALID_FILE_ATTRIBUTES;
    }

    return GetFileAttributesW(wide.Data());
}

bool IsHomeOrAbove(StringView folder) noexcept
{
    return !g_state.homeDir.Empty() && PathCovers(folder, g_state.homeDir.View());
}

} // namespace

bool SetUpProgramModules(
    StringView entry,
    String& entryResolved,
    String& message) noexcept
{
    entryResolved.Clear();
    Grant program;

    if (!CanonicalizeOptionPath(entry, "the program", program, message))
    {
        return false;
    }

    const DWORD attributes = AttributesOf(program.resolved.View());

    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        Append(message, "cannot find the program ");
        Append(message, program.resolved.View());
        return false;
    }

    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        Append(message, program.resolved.View());
        Append(message, " is a folder; give the file to run");
        return false;
    }

    const StringView folder = ParentPath(program.resolved.View());
    String root;

    for (StringView f = folder; !IsRootPath(f) && !IsHomeOrAbove(f);)
    {
        String manifest;

        if (!Append(manifest, f) || !Append(manifest, "\\") || !Append(manifest, "forge.json"))
        {
            Append(message, "out of memory while setting up module loading");
            return false;
        }

        const DWORD manifestAttributes = AttributesOf(manifest.View());

        if (manifestAttributes != INVALID_FILE_ATTRIBUTES &&
            (manifestAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            Append(root, f);
            break;
        }

        const StringView parent = ParentPath(f);

        if (parent.Size() == f.Size())
        {
            break;
        }

        f = parent;
    }

    if (root.Empty())
    {
        Append(root, IsRootPath(folder) || IsHomeOrAbove(folder) ? program.resolved.View() : folder);
    }

    if (!AddModuleRoot(root.View(), message))
    {
        return false;
    }

    entryResolved.Swap(program.resolved);
    return true;
}

Result<CheckedPath> CheckManifest(
    StringView packageRoot,
    Denial& denial) noexcept
{
    return Checker::Manifest(packageRoot, denial);
}

bool ResolvePackageFolder(
    StringView path,
    StringView baseFolder,
    String& resolved,
    String& message) noexcept
{
    resolved.Clear();
    CanonicalPath lexical;
    PathError error = LexicalCanonicalize(path, baseFolder, lexical);

    if (error == PathError::None)
    {
        error = Resolve(lexical, resolved);
    }

    if (error != PathError::None)
    {
        Append(message, "\"file:");
        Append(message, path);
        Append(message, "\" is not a valid path: ");
        Append(message, DescribePathError(error));
        return false;
    }

    const DWORD attributes = AttributesOf(resolved.View());

    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        Append(message, "there is no folder ");
        Append(message, resolved.View());
        return false;
    }

    String manifest;

    if (!Append(manifest, resolved.View()) || !Append(manifest, "\\") ||
        !Append(manifest, "forge.json"))
    {
        Append(message, "out of memory");
        return false;
    }

    const DWORD manifestAttributes = AttributesOf(manifest.View());

    if (manifestAttributes == INVALID_FILE_ATTRIBUTES ||
        (manifestAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        Append(message, "the folder ");
        Append(message, resolved.View());
        Append(message, " has no forge.json, so it is not a package");
        return false;
    }

    return true;
}

void FindInnerPackageRoot(
    StringView outerRoot,
    StringView path,
    String& inner) noexcept
{
    inner.Clear();

    if (!PathCovers(outerRoot, path) || PathEquals(outerRoot, path))
    {
        return;
    }

    for (StringView f = ParentPath(path); PathCovers(outerRoot, f) && !PathEquals(outerRoot, f);)
    {
        String manifest;

        if (!Append(manifest, f) || !Append(manifest, "\\") || !Append(manifest, "forge.json"))
        {
            Append(inner, f);  // out of memory: answer "another package", the safe side
            return;
        }

        const DWORD attributes = AttributesOf(manifest.View());

        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            Append(inner, f);
            return;
        }

        const StringView parent = ParentPath(f);

        if (parent.Size() == f.Size())
        {
            return;
        }

        f = parent;
    }
}

StringView FirstModuleRoot() noexcept
{
    return g_state.moduleRoots.Empty() ? StringView() : g_state.moduleRoots[0].resolved.View();
}

namespace
{

bool AddEnvNames(const Vector<String>& names, StringView what, Vector<String>& out,
                 String& message) noexcept
{
    for (Size i = 0; i < names.Size(); ++i)
    {
        if (!forge::core::platform::IsValidEnvName(names[i].View()))
        {
            Append(message, what);
            Append(message, " \"");
            Append(message, names[i].View());
            Append(message, "\" is not a valid variable name");
            return false;
        }

        String copy;

        if (copy.Append(names[i].View()).HasError() || out.PushBack(std::move(copy)).HasError())
        {
            Append(message, "out of memory while reading the permissions");
            return false;
        }
    }

    return true;
}

void AppendPolicyLists(String& line, const Vector<Grant>& read, const Vector<Grant>& write,
                       const Vector<String>& env) noexcept
{
    line.Append("{\"read\":").Ignore();
    AppendGrantList(line, read);
    line.Append(",\"write\":").Ignore();
    AppendGrantList(line, write);
    line.Append(",\"env\":[").Ignore();

    for (Size i = 0; i < env.Size(); ++i)
    {
        if (i > 0)
        {
            line.Append(',').Ignore();
        }

        AppendJsonString(line, env[i].View());
    }

    line.Append("]}").Ignore();
}

} // namespace

bool AddPackagePolicy(
    StringView name,
    StringView programRoot,
    const PermissionLists& declared,
    const PermissionLists& granted,
    const PackagePolicy*& out,
    String& message) noexcept
{
    out = nullptr;
    message.Clear();

    if (!g_state.initialized)
    {
        Append(message, "Kavach is not initialized");
        return false;
    }

    forge::core::Result<forge::core::memory::UniquePtr<PackagePolicy>> made =
        forge::core::memory::MakeUnique<PackagePolicy>();

    if (made.HasError() || made.Value()->name.Append(name).HasError())
    {
        Append(message, "out of memory while reading the permissions");
        return false;
    }

    PackagePolicy& policy = *made.Value();
    String declaredWhat;
    String grantedWhat;
    Append(declaredWhat, "the package's forge.json \"permissions\"");
    Append(grantedWhat, "the program's forge.json \"grants\" for the package");

    // Paths are relative to the program's folder (PHASE9_DESIGN.md, 6).
    if (!AddGrants(declared.read, declaredWhat.View(), policy.declaredRead, message, programRoot) ||
        !AddGrants(declared.write, declaredWhat.View(), policy.declaredWrite, message, programRoot) ||
        !AddEnvNames(declared.env, declaredWhat.View(), policy.declaredEnv, message) ||
        !AddGrants(granted.read, grantedWhat.View(), policy.grantedRead, message, programRoot) ||
        !AddGrants(granted.write, grantedWhat.View(), policy.grantedWrite, message, programRoot) ||
        !AddEnvNames(granted.env, grantedWhat.View(), policy.grantedEnv, message))
    {
        return false;
    }

    if (g_state.audit != INVALID_HANDLE_VALUE)
    {
        String line;
        line.Append("{\"time\":").Ignore();
        AppendTimestamp(line);
        line.Append(",\"event\":\"package\",\"package\":").Ignore();
        AppendJsonString(line, name);
        line.Append(",\"declared\":").Ignore();
        AppendPolicyLists(line, policy.declaredRead, policy.declaredWrite, policy.declaredEnv);
        line.Append(",\"granted\":").Ignore();
        AppendPolicyLists(line, policy.grantedRead, policy.grantedWrite, policy.grantedEnv);
        line.Append("}\n").Ignore();

        forge::core::LockGuard<forge::core::Mutex> lock(g_auditMutex);

        if (g_state.auditBroken || !WriteAuditLine(line))
        {
            Append(message, "the audit log could not be written (Kavach fails closed)");
            return false;
        }
    }

    out = made.Value().Get();

    if (g_state.policies.PushBack(std::move(made.Value())).HasError())
    {
        out = nullptr;
        Append(message, "out of memory while reading the permissions");
        return false;
    }

    return true;
}

bool HoldsPermission(
    const PackagePolicy* policy) noexcept
{
    if (policy == nullptr || policy->noPermission)
    {
        return false;
    }

    if (Overlaps(policy->declaredRead, policy->grantedRead) ||
        Overlaps(policy->declaredWrite, policy->grantedWrite))
    {
        return true;
    }

    for (Size i = 0; i < policy->declaredEnv.Size(); ++i)
    {
        if (HasName(policy->grantedEnv, policy->declaredEnv[i].View()))
        {
            return true;
        }
    }

    return false;
}

StringView PolicyPackageName(
    const PackagePolicy* policy) noexcept
{
    return policy == nullptr || policy->noPermission ? StringView() : policy->name.View();
}

void DescribeUngranted(
    const PackagePolicy* policy,
    String& out) noexcept
{
    out.Clear();

    if (policy == nullptr || policy->noPermission)
    {
        return;
    }

    auto add = [&out](const char* kind, StringView what)
    {
        if (!out.Empty())
        {
            Append(out, ", ");
        }

        Append(out, kind);
        Append(out, " ");
        Append(out, what);
    };

    for (int list = 0; list < 2; ++list)
    {
        const Vector<Grant>& declared = list == 0 ? policy->declaredRead : policy->declaredWrite;
        const Vector<Grant>& granted = list == 0 ? policy->grantedRead : policy->grantedWrite;

        for (Size i = 0; i < declared.Size(); ++i)
        {
            Vector<Grant> one;
            Grant copy;

            if (copy.resolved.Append(declared[i].resolved.View()).HasError() ||
                one.PushBack(std::move(copy)).HasError())
            {
                return;
            }

            if (!Overlaps(one, granted))
            {
                add(list == 0 ? "read" : "write", declared[i].resolved.View());
            }
        }
    }

    for (Size i = 0; i < policy->declaredEnv.Size(); ++i)
    {
        if (!HasName(policy->grantedEnv, policy->declaredEnv[i].View()))
        {
            add("env", policy->declaredEnv[i].View());
        }
    }
}

const PackagePolicy* NoPermissionPolicy() noexcept
{
    return &g_noPermission;
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
    return Checker::Check(nullptr, op, access, path, denial);
}

Result<CheckedPath> CheckPath(
    Op op,
    Access access,
    StringView path,
    Denial& denial) noexcept
{
    return Checker::Check(nullptr, op, access, path, denial);
}

Result<CheckedPath> CheckPath(
    const PackagePolicy* policy,
    Op op,
    Access access,
    StringView path,
    Denial& denial) noexcept
{
    return Checker::Check(policy, op, access, path, denial);
}

bool VerifyOpenedPath(
    const CheckedPath& checked,
    StringView finalPath,
    Denial& denial) noexcept
{
    denial = Denial();
    denial.access = checked.GetAccess();

    // Phase 9.2: a manifest must be exactly the file that was checked.
    if (checked.IsManifest())
    {
        if (!PathEquals(finalPath, checked.Text()))
        {
            RecordLateDenial(checked, denial, Denial::Kind::Changed, finalPath);
            return false;
        }

        return true;
    }

    if (IsProtected(finalPath))
    {
        RecordLateDenial(checked, denial, Denial::Kind::Protected, finalPath);
        return false;
    }

    // Phase 9.4: against the same policy the path was checked with.
    Denial::Layer layer = Denial::Layer::None;

    if (!Covered(checked.Policy(), checked.GetOp(), checked.GetAccess(), finalPath, true, layer))
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
    denial.package.Clear();

    if (checked.Policy() != nullptr)
    {
        denial.package.Append(AuditPackageName(checked.Policy())).Ignore();
    }

    Audit(checked.GetOp(), AuditPermissionName(checked.GetOp(), checked.GetAccess()), target, kind,
          checked.Policy());
}

Result<void> CheckEnv(
    const PackagePolicy* policy,
    Op op,
    StringView name,
    Denial& denial) noexcept
{
    denial = Denial();

    if (policy != nullptr)
    {
        denial.package.Append(AuditPackageName(policy)).Ignore();
    }

    if (!g_state.initialized || GetOpInfo(op).permission != Permission::Env)
    {
        return Refuse<void>(op, denial, Denial::Kind::Internal, name, PathError::None, policy);
    }

    const bool commandLine = g_state.allEnv || HasName(g_state.envNames, name);

    // Phase 9.4: a package needs all three layers (D26).
    if (policy != nullptr)
    {
        if (policy->noPermission || !HasName(policy->declaredEnv, name))
        {
            denial.layer = Denial::Layer::NotDeclared;
        }
        else if (!HasName(policy->grantedEnv, name))
        {
            denial.layer = Denial::Layer::NotGranted;
        }
        else if (!commandLine)
        {
            denial.layer = Denial::Layer::CommandLine;
        }
    }

    if (!commandLine || denial.layer != Denial::Layer::None)
    {
        return Refuse<void>(op, denial, Denial::Kind::NotGranted, name, PathError::None, policy);
    }

    if (!Audit(op, PermissionName(Permission::Env), name, Denial::Kind::None, policy))
    {
        denial.kind = Denial::Kind::AuditFailed;
        denial.target.Append(name).Ignore();
        return Result<void>(Failure{ Error(ErrorCode::PermissionDenied) });
    }

    return Result<void>();
}

Result<void> CheckEnvAll(
    const PackagePolicy* policy,
    Op op,
    Denial& denial) noexcept
{
    denial = Denial();

    if (policy != nullptr)
    {
        denial.package.Append(AuditPackageName(policy)).Ignore();
    }

    if (!g_state.initialized || GetOpInfo(op).permission != Permission::EnvAll)
    {
        return Refuse<void>(op, denial, Denial::Kind::Internal, "*", PathError::None, policy);
    }

    // Phase 9.4: a package reads only the variables it names.
    if (policy != nullptr)
    {
        denial.layer = Denial::Layer::NotDeclared;
        return Refuse<void>(op, denial, Denial::Kind::NotGranted, "*", PathError::None, policy);
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

namespace
{

/// `text` as the inside of a JSON string (for the grants hint).
void AppendJsonInner(String& out, StringView text) noexcept
{
    for (Size i = 0; i < text.Size(); ++i)
    {
        const char c = text.Data()[i];

        if (c == '\\' || c == '"')
        {
            out.Append('\\').Ignore();
        }

        out.Append(c).Ignore();
    }
}

/// Phase 9.4: a package's refusal, naming the layer that refused (D26) and
/// what would allow it.
void FormatPackageDenial(const OpInfo& info, const Denial& denial, String& message) noexcept
{
    const StringView target = denial.target.View();
    const StringView package = denial.package.View();
    const bool isEnv = info.permission == Permission::Env;
    const bool isEnvAll = info.permission == Permission::EnvAll;
    const bool read = denial.access == Access::Read;
    const char* kind = isEnv ? "env" : read ? "read" : "write";

    // "read access to C:\x" / "access to the environment variable HOME"
    auto what = [&](String& out)
    {
        if (isEnv)
        {
            Append(out, "access to the environment variable ");
        }
        else
        {
            Append(out, read ? "read access to " : "write access to ");
        }

        Append(out, target);
    };

    Append(message, "Permission denied: ");

    if (isEnvAll)
    {
        Append(message, "the package \"");
        Append(message, package);
        Append(message, "\" cannot read every environment variable (");
        Append(message, info.name);
        Append(message, "). A package reads only the variables its forge.json names in "
                        "\"permissions\".");
        return;
    }

    if (denial.layer == Denial::Layer::NotDeclared && package.StartsWith("("))
    {
        Append(message, info.name);
        Append(message, " needs ");
        what(message);
        Append(message, ", but it was called by a package that holds no permission.");
        return;
    }

    Append(message, "the package \"");
    Append(message, package);
    Append(message, "\" (");
    Append(message, info.name);
    Append(message, ") ");

    switch (denial.layer)
    {
    case Denial::Layer::NotDeclared:
        Append(message, "needs ");
        what(message);
        Append(message, ", which its forge.json does not declare in \"permissions\". A package "
                        "may use only what it declares.");
        return;

    case Denial::Layer::NotGranted:
        Append(message, "declares ");
        what(message);
        Append(message, ", but the program's forge.json does not grant it.\nTo allow it, add to "
                        "\"grants\" in the program's forge.json: \"");
        Append(message, package);
        Append(message, "\": { \"");
        Append(message, kind);
        Append(message, "\": [\"");
        AppendJsonInner(message, target);
        Append(message, "\"] }");
        return;

    case Denial::Layer::CommandLine:
    case Denial::Layer::None:
        Append(message, "is granted ");
        what(message);
        Append(message, " by forge.json, but the command line does not allow it.\nTo allow it, run "
                        "Forge with ");

        if (isEnv)
        {
            Append(message, "--allow-env=");
            Append(message, target);
        }
        else
        {
            Append(message, read ? "--allow-read=" : "--allow-write=");
            Append(message, target);
            Append(message, " (or a folder that contains it)");
        }

        Append(message, ".");
        return;
    }
}

} // namespace

void FormatDenial(
    Op op,
    const Denial& denial,
    String& message) noexcept
{
    const OpInfo& info = GetOpInfo(op);
    const StringView target = denial.target.View();

    if (denial.kind == Denial::Kind::NotGranted && denial.layer != Denial::Layer::None)
    {
        FormatPackageDenial(info, denial, message);
        return;
    }

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

        if (info.permission == Permission::Module)
        {
            Append(message, " cannot load ");
            Append(message, target);
            Append(message, ": a program loads modules only from its own folder (");
            Append(message, FirstModuleRoot());
            Append(message, ") and from the folders of its declared packages. Import a package by "
                            "the name listed in forge.json \"dependencies\".");
        }
        else if (info.permission == Permission::Read || info.permission == Permission::Write ||
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
