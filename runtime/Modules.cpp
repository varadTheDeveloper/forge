// ES modules (Phase 9.1). See Modules.h and PHASE9_DESIGN.md section 3.
//
// The engine's own shell loader (js/src/shell/ModuleLoader.cpp at the
// pinned commit) is the reference for how the load hook, LoadRequestedModules
// and FinishLoadingImportedModule fit together.

#include "runtime/Modules.h"

#include <utility>

#include "js/Array.h"
#include "js/CallArgs.h"
#include "js/CompileOptions.h"
#include "js/Modules.h"
#include "js/Promise.h"
#include "js/PropertyAndElement.h"
#include "js/ScriptPrivate.h"
#include "js/SourceText.h"
#include "js/TracingAPI.h"
#include "js/ValueArray.h"
#include "jsapi.h"
#include "jsfriendapi.h"

#include "forge-core/HashMap.h"
#include "forge-core/memory/MakeUnique.h"
#include "forge-core/memory/UniquePtr.h"
#include "forge-core/memory/Vector.h"
#include "kavach/Kavach.h"
#include "kavach/SecureFs.h"
#include "runtime/Inspect.h"
#include "runtime/JsUtil.h"
#include "runtime/ModuleSpecifier.h"

namespace forge::runtime
{

namespace
{

using forge::core::HashMap;
using forge::core::Result;
using forge::core::Size;
using forge::core::String;
using forge::core::StringView;
using forge::core::u8;
using forge::core::Vector;
using forge::core::memory::UniquePtr;
using forge::kavach::Op;

/// One loaded module. Records live until ShutdownModules, so a pointer to
/// one is a safe module private (JS::SetModulePrivate).
struct ModuleRecord
{
    String url;   // normalized file URL (import.meta.url)
    String path;  // canonical, resolved Windows path (the module's identity)
    JS::ModuleType type = JS::ModuleType::JavaScript;
    bool isEntry = false;
    JS::Heap<JSObject*> module;
};

struct Registry
{
    Vector<UniquePtr<ModuleRecord>> records;
    /// "<type>|<resolved path>" and "<type>|<url as first written>" -> index.
    HashMap<String, Size> byKey;
};

Registry* g_registry = nullptr;
bool g_tracing = false;

constexpr const char* kResolveName = "import.meta.resolve";

void TraceRegistry(JSTracer* trc, void* data)
{
    (void)data;

    if (g_registry == nullptr)
    {
        return;
    }

    for (Size i = 0; i < g_registry->records.Size(); ++i)
    {
        JS::TraceEdge(trc, &g_registry->records[i]->module, "forge-module");
    }
}

bool MakeKey(JS::ModuleType type, StringView text, String& key)
{
    return key.Append(type == JS::ModuleType::JSON ? 'J' : 'j').HasValue() &&
           key.Append('|').HasValue() && key.Append(text).HasValue();
}

ModuleRecord* Lookup(JS::ModuleType type, StringView text)
{
    String key;

    if (g_registry == nullptr || !MakeKey(type, text, key))
    {
        return nullptr;
    }

    const Size* index = g_registry->byKey.Find(key);
    return index != nullptr ? g_registry->records[*index].Get() : nullptr;
}

bool AddAlias(JS::ModuleType type, StringView text, Size index)
{
    String key;
    return MakeKey(type, text, key) && g_registry->byKey.Insert(key, index).HasValue();
}

/// The record of the module whose script is `referrer`, or null (a classic
/// script, a JSON module, or no referrer at all).
ModuleRecord* RecordOf(JS::HandleScript referrer)
{
    if (!referrer)
    {
        return nullptr;
    }

    // Only JavaScript modules loaded here carry a private (a PrivateValue,
    // which looks like a double); classic scripts have none.
    const JS::Value priv = JS::GetScriptPrivate(referrer);
    return priv.isDouble() ? static_cast<ModuleRecord*>(priv.toPrivate()) : nullptr;
}

/// Throws an Error with `code`, plus the specifier and the importing
/// module, so a program can tell which import failed.
bool ThrowModuleError(JSContext* cx, const char* code, StringView message, StringView specifier,
                      StringView referrerUrl)
{
    String text;

    if (text.Append(message).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    JS_ReportErrorUTF8(cx, "%s", text.CStr());
    DefineStringOnPendingError(cx, "code", StringView(code));
    DefineStringOnPendingError(cx, "specifier", specifier);

    if (!referrerUrl.Empty())
    {
        DefineStringOnPendingError(cx, "referrer", referrerUrl);
    }

    return false;
}

/// "imported from file:///C:/app/main.js" or "imported by the program".
bool AppendImporter(String& text, StringView referrerUrl)
{
    if (referrerUrl.Empty())
    {
        return text.Append(StringView("the program")).HasValue();
    }

    return text.Append(referrerUrl).HasValue();
}

bool EndsWithIgnoreCase(StringView text, const char* suffix)
{
    const StringView s(suffix);

    if (text.Size() < s.Size())
    {
        return false;
    }

    for (Size i = 0; i < s.Size(); ++i)
    {
        char c = text.Data()[text.Size() - s.Size() + i];

        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }

        if (c != s.Data()[i])
        {
            return false;
        }
    }

    return true;
}

/// Reads, compiles and registers the module at `url`, or returns the one
/// already loaded. Throws (returns null) on failure.
JSObject* LoadModule(JSContext* cx, StringView url, JS::ModuleType type, StringView specifier,
                     StringView referrerUrl, bool isEntry)
{
    if (ModuleRecord* known = Lookup(type, url))
    {
        return known->module;
    }

    String path;
    const SpecifierError pathError = FileUrlToPath(url, path);

    if (pathError != SpecifierError::None)
    {
        String text;
        text.Append(StringView("Cannot import \"")).Ignore();
        text.Append(specifier).Ignore();
        text.Append(StringView("\": ")).Ignore();
        text.Append(StringView(DescribeSpecifierError(pathError))).Ignore();
        ThrowModuleError(cx, SpecifierErrorCode(pathError), text.View(), specifier,
                                referrerUrl);
        return nullptr;
    }

    // The module root decides, never the read grants (D23).
    forge::kavach::Denial denial;
    Result<forge::kavach::CheckedPath> checked =
        forge::kavach::CheckPath(Op::ModuleImport, path.View(), denial);

    if (checked.HasError())
    {
        ThrowKavachDenial(cx, Op::ModuleImport, denial);
        DefineStringOnPendingError(cx, "specifier", specifier);

        if (!referrerUrl.Empty())
        {
            DefineStringOnPendingError(cx, "referrer", referrerUrl);
        }

        return nullptr;
    }

    const StringView resolved = checked.Value().Text();

    if (ModuleRecord* known = Lookup(type, resolved))
    {
        // Another spelling of a module already loaded (a different relative
        // path, a junction, a short name): one module, one identity.
        Size index = 0;

        while (g_registry->records[index].Get() != known)
        {
            ++index;
        }

        if (!AddAlias(type, url, index))
        {
            JS_ReportOutOfMemory(cx);
            return nullptr;
        }

        return known->module;
    }

    if (type == JS::ModuleType::JavaScript && EndsWithIgnoreCase(resolved, ".json"))
    {
        String text;
        text.Append(StringView("Cannot import \"")).Ignore();
        text.Append(specifier).Ignore();
        text.Append(StringView("\" as JavaScript: it is a JSON file. Import it with "
                               "`with { type: \"json\" }`.")).Ignore();
        ThrowModuleError(cx, "InvalidArgument", text.View(), specifier, referrerUrl);
        return nullptr;
    }

    Result<Vector<u8>> bytes = forge::kavach::fs::ReadFile(checked.Value(), denial);

    if (bytes.HasError())
    {
        if (denial.kind != forge::kavach::Denial::Kind::None)
        {
            ThrowKavachDenial(cx, Op::ModuleImport, denial);
            DefineStringOnPendingError(cx, "specifier", specifier);
            return nullptr;
        }

        const bool notFound = bytes.Error().Code() == forge::core::ErrorCode::NotFound;
        String text;
        text.Append(StringView(notFound ? "Cannot find module \"" : "Cannot read module \"")).Ignore();
        text.Append(specifier).Ignore();
        text.Append(StringView("\" imported from ")).Ignore();
        AppendImporter(text, referrerUrl);
        text.Append(StringView(notFound ? ": there is no file " : ": reading ")).Ignore();
        text.Append(resolved).Ignore();
        text.Append(StringView(notFound ? "" : " failed")).Ignore();
        ThrowModuleError(cx, ErrorCodeToString(bytes.Error().Code()), text.View(),
                                specifier, referrerUrl);
        return nullptr;
    }

    const Vector<u8>& data = bytes.Value();
    const char* source = reinterpret_cast<const char*>(data.Data());
    Size length = data.Size();

    // A UTF-8 byte-order mark is not part of the source.
    if (length >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
    {
        source += 3;
        length -= 3;
    }

    String filename;

    if (filename.Append(resolved).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    JS::CompileOptions options(cx);
    options.setFileAndLine(filename.CStr(), 1);

    JS::SourceText<mozilla::Utf8Unit> text;

    if (!text.init(cx, length == 0 ? "" : source, length, JS::SourceOwnership::Borrowed))
    {
        return nullptr;
    }

    JS::RootedObject module(cx, type == JS::ModuleType::JSON
                                    ? JS::CompileJsonModule(cx, options, text)
                                    : JS::CompileModule(cx, options, text));

    if (!module)
    {
        return nullptr;
    }

    Result<UniquePtr<ModuleRecord>> made = forge::core::memory::MakeUnique<ModuleRecord>();

    if (made.HasError() || made.Value()->url.Append(url).HasError() ||
        made.Value()->path.Append(resolved).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    ModuleRecord* record = made.Value().Get();
    record->type = type;
    record->isEntry = isEntry;
    record->module = module;

    const Size index = g_registry->records.Size();

    if (g_registry->records.PushBack(std::move(made.Value())).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    if (!AddAlias(type, resolved, index) || !AddAlias(type, url, index))
    {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    if (type == JS::ModuleType::JavaScript)
    {
        JS::SetModulePrivate(module, JS::PrivateValue(record));
    }

    return module;
}

/// Resolves `moduleRequest` against `referrer` and loads it.
JSObject* ResolveAndLoad(JSContext* cx, StringView referrerUrl, JS::HandleObject moduleRequest)
{
    JS::RootedString specifierString(cx, JS::GetModuleRequestSpecifier(cx, moduleRequest));
    String specifier;

    if (!specifierString || !AppendStringUtf8(cx, specifierString, specifier))
    {
        return nullptr;
    }

    if (JS::ModuleRequestIsSourcePhase(cx, moduleRequest))
    {
        ThrowModuleError(cx, "NotSupported",
                                StringView("Source-phase imports are not supported"),
                                specifier.View(), referrerUrl);
        return nullptr;
    }

    JS::ModuleType type = JS::GetModuleRequestType(cx, moduleRequest);

    if (type != JS::ModuleType::JavaScript && type != JS::ModuleType::JSON)
    {
        String text;
        text.Append(StringView("Cannot import \"")).Ignore();
        text.Append(specifier.View()).Ignore();
        text.Append(StringView("\": Forge loads JavaScript and JSON modules only "
                               "(`with { type: \"json\" }`)")).Ignore();
        ThrowModuleError(cx, "NotSupported", text.View(), specifier.View(), referrerUrl);
        return nullptr;
    }

    String url;
    const SpecifierError error = ResolveSpecifier(specifier.View(), referrerUrl, url);

    if (error != SpecifierError::None)
    {
        String text;
        text.Append(StringView("Cannot import \"")).Ignore();
        text.Append(specifier.View()).Ignore();
        text.Append(StringView("\" from ")).Ignore();
        AppendImporter(text, referrerUrl);
        text.Append(StringView(": ")).Ignore();
        text.Append(StringView(DescribeSpecifierError(error))).Ignore();
        ThrowModuleError(cx, SpecifierErrorCode(error), text.View(), specifier.View(),
                                referrerUrl);
        return nullptr;
    }

    return LoadModule(cx, url.View(), type, specifier.View(), referrerUrl, false);
}

StringView UrlOf(ModuleRecord* record)
{
    return record != nullptr ? record->url.View() : StringView();
}

bool OnLoadResolved(JSContext* cx, JS::HandleValue hostDefined)
{
    JS::RootedObject module(cx, &hostDefined.toObject());
    return JS::ModuleLink(cx, module);
}

bool OnLoadRejected(JSContext* cx, JS::HandleValue hostDefined, JS::HandleValue error)
{
    (void)hostDefined;
    JS_SetPendingException(cx, error);
    return true;
}

/// The deferred half of a dynamic import(): args[0] is the holder array
/// [moduleRequest, payload, referrer URL or undefined], made by
/// LoadImportedModule. The array never reaches script, so reading its
/// elements runs no script.
bool ContinueDynamicImport(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    if (!args.get(0).isObject())
    {
        return true;  // the never-taken rejection path
    }

    JS::RootedObject holder(cx, &args[0].toObject());
    JS::RootedValue requestValue(cx);
    JS::RootedValue payload(cx);
    JS::RootedValue referrerValue(cx);

    if (!JS_GetElement(cx, holder, 0, &requestValue) || !JS_GetElement(cx, holder, 1, &payload) ||
        !JS_GetElement(cx, holder, 2, &referrerValue) || !requestValue.isObject())
    {
        return false;
    }

    JS::RootedObject moduleRequest(cx, &requestValue.toObject());
    String referrerUrl;

    if (referrerValue.isString() &&
        !AppendStringUtf8(cx, referrerValue.toString(), referrerUrl))
    {
        return JS::FinishLoadingImportedModuleFailedWithPendingException(cx, payload);
    }

    JS::RootedObject module(cx, ResolveAndLoad(cx, referrerUrl.View(), moduleRequest));

    if (!module)
    {
        return JS::FinishLoadingImportedModuleFailedWithPendingException(cx, payload);
    }

    JS::RootedValue hostDefined(cx, JS::ObjectValue(*module));

    if (!JS::LoadRequestedModules(cx, module, hostDefined, OnLoadResolved, OnLoadRejected) ||
        JS_IsExceptionPending(cx))
    {
        return JS::FinishLoadingImportedModuleFailedWithPendingException(cx, payload);
    }

    return JS::FinishLoadingImportedModule(cx, nullptr, moduleRequest, payload, module, false);
}

/// JS::ModuleLoadHook.
bool LoadImportedModule(JSContext* cx, JS::Handle<JSScript*> referrer,
                        JS::Handle<JSObject*> moduleRequest, JS::Handle<JS::Value> hostDefined,
                        JS::Handle<JS::Value> payload, uint32_t lineNumber,
                        JS::ColumnNumberOneOrigin columnNumber)
{
    (void)hostDefined;
    (void)lineNumber;
    (void)columnNumber;

    const StringView referrerUrl = UrlOf(RecordOf(referrer));

    JS::RootedObject payloadObject(cx, payload.isObject() ? &payload.toObject() : nullptr);

    if (payloadObject && JS::IsPromiseObject(payloadObject))
    {
        // import(): finish in a promise job, so it is always asynchronous
        // (as in the engine's shell).
        JS::RootedValueArray<3> items(cx);
        items[0].setObject(*moduleRequest);
        items[1].set(payload);

        if (!referrerUrl.Empty())
        {
            JSString* url = FromForgeString(cx, referrerUrl);

            if (!url)
            {
                return false;
            }

            items[2].setString(url);
        }

        JS::RootedObject holder(cx, JS::NewArrayObject(cx, items));
        JS::RootedValue holderValue(cx, holder ? JS::ObjectValue(*holder) : JS::UndefinedValue());
        JS::RootedObject delay(cx, holder ? JS::CallOriginalPromiseResolve(cx, holderValue)
                                          : nullptr);
        JSFunction* continuation =
            delay ? JS_NewFunction(cx, ContinueDynamicImport, 1, 0, "import") : nullptr;
        JS::RootedObject continuationObject(
            cx, continuation ? JS_GetFunctionObject(continuation) : nullptr);

        return continuationObject &&
               JS::AddPromiseReactions(cx, delay, continuationObject, continuationObject);
    }

    JS::RootedObject module(cx, ResolveAndLoad(cx, referrerUrl, moduleRequest));

    if (!module)
    {
        return false;  // the engine turns the pending error into the load failure
    }

    return JS::FinishLoadingImportedModule(cx, referrer, moduleRequest, payload, module, false);
}

/// import.meta.resolve(specifier): the URL `specifier` would load, without
/// loading it. Only strings are accepted (nothing is converted, R24).
bool ImportMetaResolve(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (!args.get(0).isString())
    {
        return ThrowArgumentTypeError(cx, Op::ImportMetaResolve, "the specifier must be a string");
    }

    String base;
    String specifier;
    const JS::Value& baseValue = js::GetFunctionNativeReserved(&args.callee(), 0);

    if ((baseValue.isString() && !AppendStringUtf8(cx, baseValue.toString(), base)) ||
        !AppendStringUtf8(cx, args[0].toString(), specifier))
    {
        return false;
    }

    String url;
    const SpecifierError error = ResolveSpecifier(specifier.View(), base.View(), url);

    if (error != SpecifierError::None)
    {
        String text;
        text.Append(StringView(kResolveName)).Ignore();
        text.Append(StringView(": cannot resolve \"")).Ignore();
        text.Append(specifier.View()).Ignore();
        text.Append(StringView("\": ")).Ignore();
        text.Append(StringView(DescribeSpecifierError(error))).Ignore();
        return ThrowModuleError(cx, SpecifierErrorCode(error), text.View(), specifier.View(),
                                base.View());
    }

    JSString* result = FromForgeString(cx, url.View());

    if (!result)
    {
        return false;
    }

    args.rval().setString(result);
    return true;
}

/// JS::ModuleMetadataHook: import.meta.url, .main and .resolve.
bool PopulateImportMeta(JSContext* cx, JS::Handle<JSObject*> moduleRecord,
                        JS::Handle<JSObject*> metaObject)
{
    const JS::Value priv = JS::GetModulePrivate(moduleRecord);
    ModuleRecord* record = priv.isDouble() ? static_cast<ModuleRecord*>(priv.toPrivate()) : nullptr;

    if (record == nullptr)
    {
        return true;
    }

    JS::RootedString url(cx, FromForgeString(cx, record->url.View()));

    if (!url)
    {
        return false;
    }

    JS::RootedValue urlValue(cx, JS::StringValue(url));
    JS::RootedValue mainValue(cx, JS::BooleanValue(record->isEntry));

    if (!JS_DefineProperty(cx, metaObject, "url", urlValue, JSPROP_ENUMERATE) ||
        !JS_DefineProperty(cx, metaObject, "main", mainValue, JSPROP_ENUMERATE))
    {
        return false;
    }

    JSFunction* resolve =
        js::DefineFunctionWithReserved(cx, metaObject, "resolve", ImportMetaResolve, 1,
                                       JSPROP_ENUMERATE);

    if (!resolve)
    {
        return false;
    }

    js::SetFunctionNativeReserved(JS_GetFunctionObject(resolve), 0, urlValue);
    return true;
}

/// The rejection handler for an entry module whose top-level await fails.
bool OnEntryRejected(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    if (!HostExitRequested(cx))
    {
        JS_SetPendingException(cx, args.get(0));
        HostReportFatal(cx, "the main module");
    }

    return true;
}

} // namespace

bool InitModules(JSContext* cx)
{
    if (g_registry == nullptr)
    {
        Result<UniquePtr<Registry>> made = forge::core::memory::MakeUnique<Registry>();

        if (made.HasError())
        {
            return false;
        }

        g_registry = made.Value().Release();
    }

    if (!g_tracing)
    {
        if (!JS_AddExtraGCRootsTracer(cx, TraceRegistry, nullptr))
        {
            return false;
        }

        g_tracing = true;
    }

    JSRuntime* rt = JS_GetRuntime(cx);
    JS::SetModuleLoadHook(rt, LoadImportedModule);
    JS::SetModuleMetadataHook(rt, PopulateImportMeta);
    return true;
}

void ShutdownModules(JSContext* cx)
{
    if (g_tracing)
    {
        JS_RemoveExtraGCRootsTracer(cx, TraceRegistry, nullptr);
        g_tracing = false;
    }

    delete g_registry;
    g_registry = nullptr;
}

EntryResult RunEntryModule(JSContext* cx, StringView entryPath)
{
    String url;

    if (!PathToFileUrl(entryPath, url))
    {
        String path;

        if (path.Append(entryPath).HasError())
        {
            JS_ReportOutOfMemory(cx);
            return EntryResult::Threw;
        }

        JS_ReportErrorUTF8(cx, "Forge cannot run %s: a program must be on a drive, not a "
                               "network share",
                           path.CStr());
        return EntryResult::Threw;
    }

    JS::RootedObject module(cx, LoadModule(cx, url.View(), JS::ModuleType::JavaScript,
                                           url.View(), StringView(), true));

    if (!module)
    {
        return EntryResult::Threw;
    }

    JS::RootedValue hostDefined(cx, JS::ObjectValue(*module));

    if (!JS::LoadRequestedModules(cx, module, hostDefined, OnLoadResolved, OnLoadRejected) ||
        JS_IsExceptionPending(cx))
    {
        return EntryResult::Threw;
    }

    JS::RootedValue result(cx);

    if (!JS::ModuleEvaluate(cx, module, &result))
    {
        return EntryResult::Threw;
    }

    JS::RootedObject promise(cx, result.isObject() ? &result.toObject() : nullptr);

    if (!promise || !JS::IsPromiseObject(promise))
    {
        return EntryResult::Running;
    }

    switch (JS::GetPromiseState(promise))
    {
    case JS::PromiseState::Fulfilled:
        return EntryResult::Running;

    case JS::PromiseState::Rejected:
    {
        JS::RootedValue reason(cx, JS::GetPromiseResult(promise));

        if (!JS::SetSettledPromiseIsHandled(cx, promise))
        {
            return EntryResult::Threw;
        }

        // After Forge.exit() the engine rejects with undefined and nothing
        // is pending; the caller sees the exit request.
        if (!HostExitRequested(cx))
        {
            JS_SetPendingException(cx, reason);
        }

        return EntryResult::Threw;
    }

    case JS::PromiseState::Pending:
    {
        JSFunction* onRejected = JS_NewFunction(cx, OnEntryRejected, 1, 0, "onEntryRejected");
        JS::RootedObject handler(cx, onRejected ? JS_GetFunctionObject(onRejected) : nullptr);

        if (!handler || !JS::AddPromiseReactions(cx, promise, nullptr, handler))
        {
            return EntryResult::Threw;
        }

        return EntryResult::Running;
    }
    }

    return EntryResult::Running;
}

} // namespace forge::runtime
