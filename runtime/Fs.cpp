// globalThis.fs (Phase 8.3). See Fs.h for the API.
//
// Every function follows the same shape:
//   1. Check the path argument(s) with CheckPathArg (strings only, Kavach).
//   2. Parse the remaining arguments.
//   3. Call kavach/SecureFs, which works on handles and re-checks the
//      opened handle, so nothing changed on disk after step 1 can widen
//      what the call reaches.
//   4. Convert the result to JavaScript, or throw (sync) / reject (async).
//
// The Promise-returning functions run step 3 on the shared worker pool and
// settle their Promise on the main thread through an IoLoop completion,
// like the Phase 7.5 design (FsAsync.md). A refusal in step 1 or 2 is a
// rejected Promise, never a synchronous throw (Phase 8.3).

#include "runtime/Fs.h"

#include <utility>

#include "js/Array.h"
#include "js/CallArgs.h"
#include "js/Conversions.h"
#include "js/Promise.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/Proxy.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "js/ValueArray.h"
#include "jsapi.h"

#include "forge-core/HashMap.h"
#include "forge-core/memory/MakeUnique.h"
#include "forge-core/memory/UniquePtr.h"
#include "kavach/SecureFs.h"
#include "runtime/Inspect.h"
#include "runtime/JsUtil.h"

namespace forge::runtime
{

namespace
{

using forge::core::Error;
using forge::core::ErrorCode;
using forge::core::Result;
using forge::core::Size;
using forge::core::String;
using forge::core::StringView;
using forge::core::u8;
using forge::core::Vector;
using forge::kavach::Access;
using forge::kavach::CheckedPath;
using forge::kavach::Denial;
using forge::kavach::Op;
namespace sfs = forge::kavach::fs;

//------------------------------------------------------------------------------
// Small helpers
//------------------------------------------------------------------------------

/// "fs.readFileSync" -> "readFileSync", the `syscall` on Forge's errors.
const char* ShortName(Op op)
{
    const char* name = forge::kavach::GetOpInfo(op).name;
    return (name[0] == 'f' && name[1] == 's' && name[2] == '.') ? name + 3 : name;
}

/// Throws the error for a failed SecureFs call: Kavach's message for a
/// refusal, otherwise Forge's usual "<syscall> failed: <code>" error.
bool ThrowFsError(JSContext* cx, Op op, const Error& error, const Denial& denial,
                  const CheckedPath* path)
{
    if (error == ErrorCode::PermissionDenied && denial.kind != Denial::Kind::None)
    {
        return ThrowKavachDenial(cx, op, denial);
    }

    ThrowJsError(cx, error, ShortName(op), path ? &path->Path() : nullptr);
    return false;
}

bool ThrowInvalidArgument(JSContext* cx, Op op, const CheckedPath* path)
{
    ThrowJsError(cx, Error(ErrorCode::InvalidArgument), ShortName(op),
                 path ? &path->Path() : nullptr);
    return false;
}

/// `encoding`: undefined (bytes), or "utf8" / "utf-8" (a string).
bool ParseEncoding(JSContext* cx, Op op, JS::HandleValue value, const CheckedPath& path,
                   bool* utf8)
{
    *utf8 = false;

    if (value.isUndefined())
    {
        return true;
    }

    if (value.isString())
    {
        String text;

        if (!AppendStringUtf8(cx, value.toString(), text))
        {
            return false;
        }

        if (text.View() == StringView("utf8") || text.View() == StringView("utf-8"))
        {
            *utf8 = true;
            return true;
        }
    }

    return ThrowInvalidArgument(cx, op, &path);
}

/// `{ name: true }` options: undefined means false; anything that is not an
/// object is an InvalidArgument error.
///
/// Only an own data property counts (Phase 8.4, R24). Inherited properties
/// are ignored, so `Object.prototype.recursive = true` cannot turn rmSync
/// into a recursive delete; getters are never called and proxies are
/// refused, so reading the options never runs script code. JS::ToBoolean
/// runs no script either.
bool ParseFlag(JSContext* cx, Op op, JS::HandleValue options, const char* name,
               const CheckedPath& path, bool* flag)
{
    *flag = false;

    if (options.isUndefined())
    {
        return true;
    }

    if (!options.isObject() || js::IsProxy(&options.toObject()))
    {
        return ThrowInvalidArgument(cx, op, &path);
    }

    JS::RootedObject object(cx, &options.toObject());
    JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx);

    if (!JS_GetOwnPropertyDescriptor(cx, object, name, &desc))
    {
        return false;
    }

    if (desc.isNothing())
    {
        return true;
    }

    if (!desc->isDataDescriptor())
    {
        return ThrowInvalidArgument(cx, op, &path);
    }

    JS::RootedValue value(cx, desc->value());
    *flag = JS::ToBoolean(value);
    return true;
}

/// Copies write data (string as UTF-8, Uint8Array, ArrayBuffer) into an
/// owned buffer, so it can also be handed to a worker thread.
bool CopyWriteData(JSContext* cx, Op op, JS::HandleValue value, const CheckedPath& path,
                   Vector<u8>& out)
{
    if (value.isString())
    {
        String text;

        if (!AppendStringUtf8(cx, value.toString(), text))
        {
            return false;
        }

        if (out.Reserve(text.Size()).HasError())
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }

        for (Size i = 0; i < text.Size(); ++i)
        {
            out.PushBack(static_cast<u8>(text.Data()[i])).Ignore();
        }

        return true;
    }

    Result<forge::core::Span<const u8>> bytes = AsByteSpan(cx, value);

    if (bytes.HasError())
    {
        return ThrowInvalidArgument(cx, op, &path);
    }

    if (out.Reserve(bytes.Value().Size()).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    for (Size i = 0; i < bytes.Value().Size(); ++i)
    {
        out.PushBack(bytes.Value().Data()[i]).Ignore();
    }

    return true;
}

const char* TypeName(sfs::EntryType type)
{
    switch (type)
    {
    case sfs::EntryType::File: return "file";
    case sfs::EntryType::Directory: return "directory";
    case sfs::EntryType::Symlink: return "symlink";
    case sfs::EntryType::Other: return "other";
    }

    return "other";
}

bool DefineString(JSContext* cx, JS::HandleObject object, const char* name, StringView value)
{
    JS::RootedString str(cx, FromForgeString(cx, value));

    if (!str)
    {
        return false;
    }

    JS::RootedValue v(cx, JS::StringValue(str));
    return JS_DefineProperty(cx, object, name, v, JSPROP_ENUMERATE);
}

bool DefineBool(JSContext* cx, JS::HandleObject object, const char* name, bool value)
{
    JS::RootedValue v(cx, JS::BooleanValue(value));
    return JS_DefineProperty(cx, object, name, v, JSPROP_ENUMERATE);
}

bool DefineNumber(JSContext* cx, JS::HandleObject object, const char* name, double value)
{
    JS::RootedValue v(cx, JS::NumberValue(value));
    return JS_DefineProperty(cx, object, name, v, JSPROP_ENUMERATE);
}

bool StatToValue(JSContext* cx, const sfs::Stat& stat, JS::MutableHandleValue out)
{
    JS::RootedObject object(cx, JS_NewPlainObject(cx));

    if (!object || !DefineNumber(cx, object, "size", static_cast<double>(stat.size)) ||
        !DefineString(cx, object, "type", TypeName(stat.type)) ||
        !DefineBool(cx, object, "isFile", stat.type == sfs::EntryType::File) ||
        !DefineBool(cx, object, "isDirectory", stat.type == sfs::EntryType::Directory) ||
        !DefineBool(cx, object, "isSymbolicLink", stat.type == sfs::EntryType::Symlink) ||
        !DefineNumber(cx, object, "nlink", stat.links) ||
        !DefineNumber(cx, object, "mtimeMs", stat.mtimeMs) ||
        !DefineNumber(cx, object, "atimeMs", stat.atimeMs) ||
        !DefineNumber(cx, object, "ctimeMs", stat.ctimeMs) ||
        !DefineNumber(cx, object, "birthtimeMs", stat.birthtimeMs))
    {
        return false;
    }

    out.setObject(*object);
    return true;
}

bool EntriesToValue(JSContext* cx, const Vector<sfs::DirEntry>& entries, bool withFileTypes,
                    JS::MutableHandleValue out)
{
    JS::RootedObject array(cx, JS::NewArrayObject(cx, entries.Size()));

    if (!array)
    {
        return false;
    }

    JS::RootedValue item(cx);

    for (Size i = 0; i < entries.Size(); ++i)
    {
        if (withFileTypes)
        {
            JS::RootedObject entry(cx, JS_NewPlainObject(cx));

            if (!entry || !DefineString(cx, entry, "name", entries[i].name.View()) ||
                !DefineString(cx, entry, "type", TypeName(entries[i].type)))
            {
                return false;
            }

            item.setObject(*entry);
        }
        else
        {
            JSString* name = FromForgeString(cx, entries[i].name.View());

            if (!name)
            {
                return false;
            }

            item.setString(name);
        }

        if (!JS_DefineElement(cx, array, static_cast<uint32_t>(i), item, JSPROP_ENUMERATE))
        {
            return false;
        }
    }

    out.setObject(*array);
    return true;
}

bool BytesToValue(JSContext* cx, Vector<u8> bytes, bool utf8, JS::MutableHandleValue out)
{
    if (utf8)
    {
        JSString* text = FromForgeString(
            cx, StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()));

        if (!text)
        {
            return false;
        }

        out.setString(text);
        return true;
    }

    JSObject* array = Uint8ArrayFromBytes(cx, std::move(bytes));

    if (!array)
    {
        return false;
    }

    out.setObject(*array);
    return true;
}

//------------------------------------------------------------------------------
// One operation, described the same way for the sync and async paths
//------------------------------------------------------------------------------

enum class Kind
{
    ReadFile,
    WriteFile,
    AppendFile,
    Exists,
    Stat,
    Lstat,
    Readdir,
    Realpath,
    Mkdir,
    Rm,
    Rename,
    CopyFile,
};

struct Job
{
    // Must stay the first member: the completion pointer is cast back to
    // the Job (the same idiom as Phase 7.5's AsyncFsReadOp).
    forge::core::platform::IoCompletion completion{};

    JSContext* cx = nullptr;
    JS::Heap<JSObject*> promise;

    Kind kind = Kind::ReadFile;
    Op op = Op::Count;
    CheckedPath path;
    CheckedPath path2;     // Rename / CopyFile destination
    Vector<u8> data;       // WriteFile / AppendFile input
    bool flag = false;     // utf8, recursive or withFileTypes

    // Results, written by whoever runs the job.
    bool failed = false;
    Error error;
    Denial denial;
    Vector<u8> bytes;
    bool boolean = false;
    sfs::Stat stat;
    Vector<sfs::DirEntry> entries;
    String text;
};

/// Step 3: runs on the main thread (sync) or a worker thread (async).
void RunJob(Job& job) noexcept
{
    auto fail = [&job](const Error& error) {
        job.failed = true;
        job.error = error;
    };

    switch (job.kind)
    {
    case Kind::ReadFile:
    {
        Result<Vector<u8>> result = sfs::ReadFile(job.path, job.denial);
        result.HasError() ? fail(result.Error()) : void(job.bytes = std::move(result.Value()));
        break;
    }
    case Kind::WriteFile:
    case Kind::AppendFile:
    {
        Result<void> result = sfs::WriteFile(
            job.path, forge::core::Span<const u8>(job.data.Data(), job.data.Size()),
            job.kind == Kind::AppendFile ? sfs::WriteMode::Append : sfs::WriteMode::Truncate,
            job.denial);

        if (result.HasError())
        {
            fail(result.Error());
        }

        break;
    }
    case Kind::Exists:
    {
        Result<bool> result = sfs::Exists(job.path, job.denial);
        result.HasError() ? fail(result.Error()) : void(job.boolean = result.Value());
        break;
    }
    case Kind::Stat:
    case Kind::Lstat:
    {
        Result<sfs::Stat> result = sfs::GetStat(job.path, job.kind == Kind::Stat, job.denial);
        result.HasError() ? fail(result.Error()) : void(job.stat = result.Value());
        break;
    }
    case Kind::Readdir:
    {
        Result<Vector<sfs::DirEntry>> result = sfs::ReadDirectory(job.path, job.denial);
        result.HasError() ? fail(result.Error()) : void(job.entries = std::move(result.Value()));
        break;
    }
    case Kind::Realpath:
    {
        Result<String> result = sfs::RealPath(job.path, job.denial);
        result.HasError() ? fail(result.Error()) : void(job.text = std::move(result.Value()));
        break;
    }
    case Kind::Mkdir:
    {
        Result<void> result = sfs::MakeDirectory(job.path, job.flag, job.denial);

        if (result.HasError())
        {
            fail(result.Error());
        }

        break;
    }
    case Kind::Rm:
    {
        Result<void> result = sfs::Remove(job.path, job.flag, job.denial);

        if (result.HasError())
        {
            fail(result.Error());
        }

        break;
    }
    case Kind::Rename:
    {
        Result<void> result = sfs::Rename(job.path, job.path2, job.denial);

        if (result.HasError())
        {
            fail(result.Error());
        }

        break;
    }
    case Kind::CopyFile:
    {
        Result<void> result = sfs::CopyFile(job.path, job.path2, job.denial);

        if (result.HasError())
        {
            fail(result.Error());
        }

        break;
    }
    }
}

/// Step 4: the JavaScript value for a finished job, or false with an
/// exception pending. Main thread only.
bool JobResult(JSContext* cx, Job& job, JS::MutableHandleValue out)
{
    if (job.failed)
    {
        // A copy refused at its destination names the destination.
        const bool destination =
            job.kind == Kind::CopyFile && job.denial.kind != Denial::Kind::None &&
            job.denial.access == Access::Write;
        return ThrowFsError(cx, job.op, job.error, job.denial,
                            destination ? &job.path2 : &job.path);
    }

    switch (job.kind)
    {
    case Kind::ReadFile:
        return BytesToValue(cx, std::move(job.bytes), job.flag, out);
    case Kind::Exists:
        out.setBoolean(job.boolean);
        return true;
    case Kind::Stat:
    case Kind::Lstat:
        return StatToValue(cx, job.stat, out);
    case Kind::Readdir:
        return EntriesToValue(cx, job.entries, job.flag, out);
    case Kind::Realpath:
    {
        JSString* str = FromForgeString(cx, job.text.View());

        if (!str)
        {
            return false;
        }

        out.setString(str);
        return true;
    }
    default:
        out.setUndefined();
        return true;
    }
}

//------------------------------------------------------------------------------
// Async plumbing
//------------------------------------------------------------------------------

forge::core::HashMap<Job*, forge::core::memory::UniquePtr<Job>>* g_jobs = nullptr;
bool g_tracing = false;

void TraceJobs(JSTracer* trc, void* data)
{
    (void)data;

    if (!g_jobs)
    {
        return;
    }

    for (auto entry : *g_jobs)
    {
        JS::TraceEdge(trc, &entry.Value()->promise, "forge-fs-job-promise");
    }
}

void JobCompleted(forge::core::platform::IoCompletion* self, DWORD, ULONG_PTR, bool,
                  DWORD) noexcept
{
    auto* job = reinterpret_cast<Job*>(self);
    JSContext* cx = job->cx;
    JS::RootedObject promise(cx, job->promise);
    JS::RootedValue value(cx);

    {
        JSAutoRealm ar(cx, promise);

        if (JobResult(cx, *job, &value))
        {
            if (!JS::ResolvePromise(cx, promise, value))
            {
                JS_ClearPendingException(cx);
            }
        }
        else
        {
            TakePendingException(cx, &value);

            if (!JS::RejectPromise(cx, promise, value))
            {
                JS_ClearPendingException(cx);
            }
        }
    }

    HostAsyncFinished(cx);

    if (g_jobs)
    {
        g_jobs->Erase(job);  // frees the job; nothing may touch it after this
    }
}

/// Converts a failure before dispatch (bad argument, Kavach refusal) into
/// a rejected Promise.
bool RejectNow(JSContext* cx, JS::CallArgs& args)
{
    if (!JS_IsExceptionPending(cx))
    {
        return false;  // uncatchable (Forge.exit, out of memory): propagate
    }

    JS::RootedValue exception(cx);
    TakePendingException(cx, &exception);
    JSObject* rejected = JS::CallOriginalPromiseReject(cx, exception);

    if (!rejected)
    {
        return false;
    }

    args.rval().setObject(*rejected);
    return true;
}

/// Hands `job` to the worker pool and returns its Promise.
bool Dispatch(JSContext* cx, JS::CallArgs& args, forge::core::memory::UniquePtr<Job> owned)
{
    JS::RootedObject promise(cx, JS::NewPromiseObject(cx, nullptr));

    if (!promise || !g_jobs)
    {
        if (!JS_IsExceptionPending(cx))
        {
            JS_ReportOutOfMemory(cx);
        }

        return false;
    }

    owned->cx = cx;
    owned->promise = promise;
    owned->completion.callback = &JobCompleted;
    Job* job = owned.Get();

    if (g_jobs->Insert(job, std::move(owned)).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    forge::core::platform::IoLoop* loop = &HostIoLoop(cx);
    HostAsyncStarted(cx);

    Result<void> submitted = HostWorkerPool(cx).Submit([job, loop]() {
        RunJob(*job);
        // If posting fails the Promise never settles; there is no thread
        // left to report to. PostQueuedCompletionStatus fails only when the
        // port is gone, which means the loop is shutting down.
        loop->PostCompletion(&job->completion, 0, 0).Ignore();
    });

    if (submitted.HasError())
    {
        HostAsyncFinished(cx);
        g_jobs->Erase(job);
        JS_ReportOutOfMemory(cx);
        return false;
    }

    args.rval().setObject(*promise);
    return true;
}

forge::core::memory::UniquePtr<Job> NewJob(Kind kind, Op op)
{
    Result<forge::core::memory::UniquePtr<Job>> made = forge::core::memory::MakeUnique<Job>();

    if (made.HasError())
    {
        return forge::core::memory::UniquePtr<Job>();
    }

    made.Value()->kind = kind;
    made.Value()->op = op;
    return std::move(made.Value());
}

//------------------------------------------------------------------------------
// Argument parsing shared by each sync/async pair. Returns the job, or null
// with an exception pending.
//------------------------------------------------------------------------------

forge::core::memory::UniquePtr<Job> PrepareJob(JSContext* cx, JS::CallArgs& args, Kind kind, Op op)
{
    forge::core::memory::UniquePtr<Job> none;

    if (!args.requireAtLeast(cx, ShortName(op),
                             (kind == Kind::WriteFile || kind == Kind::AppendFile ||
                              kind == Kind::Rename || kind == Kind::CopyFile)
                                 ? 2
                                 : 1))
    {
        return none;
    }

    const Access firstAccess = kind == Kind::CopyFile ? Access::Read
                             : forge::kavach::GetOpInfo(op).permission == forge::kavach::Permission::Write
                                 ? Access::Write
                                 : Access::Read;
    Result<CheckedPath> path = CheckPathArg(cx, op, firstAccess, args[0]);

    if (path.HasError())
    {
        return none;
    }

    forge::core::memory::UniquePtr<Job> job = NewJob(kind, op);

    if (!job)
    {
        JS_ReportOutOfMemory(cx);
        return none;
    }

    job->path = std::move(path.Value());

    switch (kind)
    {
    case Kind::ReadFile:
        if (!ParseEncoding(cx, op, args.get(1), job->path, &job->flag))
        {
            return none;
        }
        break;

    case Kind::WriteFile:
    case Kind::AppendFile:
        if (!CopyWriteData(cx, op, args[1], job->path, job->data))
        {
            return none;
        }
        break;

    case Kind::Readdir:
        if (!ParseFlag(cx, op, args.get(1), "withFileTypes", job->path, &job->flag))
        {
            return none;
        }
        break;

    case Kind::Mkdir:
    case Kind::Rm:
        if (!ParseFlag(cx, op, args.get(1), "recursive", job->path, &job->flag))
        {
            return none;
        }
        break;

    case Kind::Rename:
    case Kind::CopyFile:
    {
        Result<CheckedPath> second = CheckPathArg(cx, op, Access::Write, args[1]);

        if (second.HasError())
        {
            return none;
        }

        job->path2 = std::move(second.Value());
        break;
    }

    default:
        break;
    }

    return job;
}

bool RunSync(JSContext* cx, unsigned argc, JS::Value* vp, Kind kind, Op op)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    forge::core::memory::UniquePtr<Job> job = PrepareJob(cx, args, kind, op);

    if (!job)
    {
        return false;
    }

    RunJob(*job);
    JS::RootedValue value(cx);

    if (!JobResult(cx, *job, &value))
    {
        return false;
    }

    args.rval().set(value);
    return true;
}

bool RunAsync(JSContext* cx, unsigned argc, JS::Value* vp, Kind kind, Op op)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    forge::core::memory::UniquePtr<Job> job = PrepareJob(cx, args, kind, op);

    if (!job)
    {
        return RejectNow(cx, args);
    }

    return Dispatch(cx, args, std::move(job));
}

#define FORGE_FS_NATIVE(Name, KindValue, OpValue, RunFn)                   \
    bool Name(JSContext* cx, unsigned argc, JS::Value* vp)                 \
    {                                                                      \
        return RunFn(cx, argc, vp, Kind::KindValue, Op::OpValue);          \
    }

FORGE_FS_NATIVE(ReadFileSync, ReadFile, FsReadFileSync, RunSync)
FORGE_FS_NATIVE(ReadFileAsync, ReadFile, FsReadFile, RunAsync)
FORGE_FS_NATIVE(WriteFileSync, WriteFile, FsWriteFileSync, RunSync)
FORGE_FS_NATIVE(WriteFileAsync, WriteFile, FsWriteFile, RunAsync)
FORGE_FS_NATIVE(AppendFileSync, AppendFile, FsAppendFileSync, RunSync)
FORGE_FS_NATIVE(AppendFileAsync, AppendFile, FsAppendFile, RunAsync)
FORGE_FS_NATIVE(ExistsSync, Exists, FsExistsSync, RunSync)
FORGE_FS_NATIVE(ExistsAsync, Exists, FsExists, RunAsync)
FORGE_FS_NATIVE(StatSync, Stat, FsStatSync, RunSync)
FORGE_FS_NATIVE(StatAsync, Stat, FsStat, RunAsync)
FORGE_FS_NATIVE(LstatSync, Lstat, FsLstatSync, RunSync)
FORGE_FS_NATIVE(LstatAsync, Lstat, FsLstat, RunAsync)
FORGE_FS_NATIVE(ReaddirSync, Readdir, FsReaddirSync, RunSync)
FORGE_FS_NATIVE(ReaddirAsync, Readdir, FsReaddir, RunAsync)
FORGE_FS_NATIVE(RealpathSync, Realpath, FsRealpathSync, RunSync)
FORGE_FS_NATIVE(RealpathAsync, Realpath, FsRealpath, RunAsync)
FORGE_FS_NATIVE(MkdirSync, Mkdir, FsMkdirSync, RunSync)
FORGE_FS_NATIVE(MkdirAsync, Mkdir, FsMkdir, RunAsync)
FORGE_FS_NATIVE(RmSync, Rm, FsRmSync, RunSync)
FORGE_FS_NATIVE(RmAsync, Rm, FsRm, RunAsync)
FORGE_FS_NATIVE(RenameSync, Rename, FsRenameSync, RunSync)
FORGE_FS_NATIVE(RenameAsync, Rename, FsRename, RunAsync)
FORGE_FS_NATIVE(CopyFileSync, CopyFile, FsCopyFileSync, RunSync)
FORGE_FS_NATIVE(CopyFileAsync, CopyFile, FsCopyFile, RunAsync)

#undef FORGE_FS_NATIVE

const JSFunctionSpec kFsFunctions[] = {
    JS_FN("readFileSync", ReadFileSync, 2, JSPROP_ENUMERATE),
    JS_FN("readFile", ReadFileAsync, 2, JSPROP_ENUMERATE),
    JS_FN("writeFileSync", WriteFileSync, 2, JSPROP_ENUMERATE),
    JS_FN("writeFile", WriteFileAsync, 2, JSPROP_ENUMERATE),
    JS_FN("appendFileSync", AppendFileSync, 2, JSPROP_ENUMERATE),
    JS_FN("appendFile", AppendFileAsync, 2, JSPROP_ENUMERATE),
    JS_FN("existsSync", ExistsSync, 1, JSPROP_ENUMERATE),
    JS_FN("exists", ExistsAsync, 1, JSPROP_ENUMERATE),
    JS_FN("statSync", StatSync, 1, JSPROP_ENUMERATE),
    JS_FN("stat", StatAsync, 1, JSPROP_ENUMERATE),
    JS_FN("lstatSync", LstatSync, 1, JSPROP_ENUMERATE),
    JS_FN("lstat", LstatAsync, 1, JSPROP_ENUMERATE),
    JS_FN("readdirSync", ReaddirSync, 2, JSPROP_ENUMERATE),
    JS_FN("readdir", ReaddirAsync, 2, JSPROP_ENUMERATE),
    JS_FN("realpathSync", RealpathSync, 1, JSPROP_ENUMERATE),
    JS_FN("realpath", RealpathAsync, 1, JSPROP_ENUMERATE),
    JS_FN("mkdirSync", MkdirSync, 2, JSPROP_ENUMERATE),
    JS_FN("mkdir", MkdirAsync, 2, JSPROP_ENUMERATE),
    JS_FN("rmSync", RmSync, 2, JSPROP_ENUMERATE),
    JS_FN("rm", RmAsync, 2, JSPROP_ENUMERATE),
    JS_FN("renameSync", RenameSync, 2, JSPROP_ENUMERATE),
    JS_FN("rename", RenameAsync, 2, JSPROP_ENUMERATE),
    JS_FN("copyFileSync", CopyFileSync, 2, JSPROP_ENUMERATE),
    JS_FN("copyFile", CopyFileAsync, 2, JSPROP_ENUMERATE),
    JS_FS_END,
};

} // namespace

bool DefineFs(JSContext* cx, JS::HandleObject global)
{
    if (!g_jobs)
    {
        static forge::core::HashMap<Job*, forge::core::memory::UniquePtr<Job>> jobs;
        g_jobs = &jobs;
    }

    if (!g_tracing)
    {
        if (!JS_AddExtraGCRootsTracer(cx, TraceJobs, nullptr))
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }

        g_tracing = true;
    }

    JS::RootedObject fs(cx, JS_NewPlainObject(cx));

    if (!fs || !JS_DefineFunctions(cx, fs, kFsFunctions))
    {
        return false;
    }

    return JS_DefineProperty(cx, global, "fs", fs, 0);
}

void ShutdownFs(JSContext* cx)
{
    if (g_jobs)
    {
        g_jobs->Clear();
    }

    if (g_tracing)
    {
        JS_RemoveExtraGCRootsTracer(cx, TraceJobs, nullptr);
        g_tracing = false;
    }
}

} // namespace forge::runtime
