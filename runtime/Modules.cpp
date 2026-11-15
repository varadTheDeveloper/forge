// ES modules (Phase 9.1). See Modules.h and PHASE9_DESIGN.md section 3.
// Packages (Phase 9.2, section 4) and compartments (Phase 9.3, section 5).
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
#include "runtime/Compartments.h"
#include "runtime/Console.h"
#include "runtime/Inspect.h"
#include "runtime/JsUtil.h"
#include "runtime/Manifest.h"
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
    Size package = 0;  // index into Registry::packages (Phase 9.2)
    Size compartment = kAppCompartment;  // where the module was compiled (Phase 9.3)
    /// Phase 9.3: a facade module, made for an importer in `compartment`,
    /// whose exports are those of records[facadeTarget] (another package).
    bool isFacade = false;
    Size facadeTarget = 0;
    JS::Heap<JSObject*> module;
};

/// Phase 9.2: a package, the app's own (index 0) or a dependency. Every
/// module belongs to exactly one package: the one whose folder holds it,
/// with no other package folder in between.
struct Package
{
    String root;         // canonical, resolved folder (or the entry file, see rootIsFile)
    String rootUrl;      // file URL of `root`
    bool isApp = false;
    bool rootIsFile = false;  // an app run from the home folder or a drive root
    bool hasManifest = false;
    Size compartment = kAppCompartment;  // Phase 9.3 (D25)
    Manifest manifest;
};

struct Registry
{
    Vector<UniquePtr<ModuleRecord>> records;
    /// "<type>|<resolved path>" and "<type>|<url as first written>" -> index.
    HashMap<String, Size> byKey;
    Vector<UniquePtr<Package>> packages;
    HashMap<String, Size> packageByRoot;
    /// Phase 9.3: the packages whose module graphs are being loaded for a
    /// facade, innermost last. A package already on it is an import cycle
    /// between packages.
    Vector<Size> loadingPackages;
};

Registry* g_registry = nullptr;
bool g_tracing = false;

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

/// Phase 9.2: true if `resolved` belongs to package `index`.
bool InsidePackage(Size index, StringView resolved)
{
    const Package& package = *g_registry->packages[index];

    if (package.rootIsFile)
    {
        return forge::kavach::PathEquals(package.root.View(), resolved);
    }

    if (!forge::kavach::PathCovers(package.root.View(), resolved))
    {
        return false;
    }

    String inner;
    forge::kavach::FindInnerPackageRoot(package.root.View(), resolved, inner);
    return inner.Empty();
}

/// "the package \"greet\" (C:\app\vendor\greet)" or "the program (C:\app)".
void AppendPackageLabel(String& text, Size index)
{
    const Package& package = *g_registry->packages[index];

    if (package.isApp)
    {
        text.Append(StringView("the program (")).Ignore();
    }
    else
    {
        text.Append(StringView("the package \"")).Ignore();
        text.Append(package.manifest.name.View()).Ignore();
        text.Append(StringView("\" (")).Ignore();
    }

    text.Append(package.root.View()).Ignore();
    text.Append(')').Ignore();
}

bool ThrowBoundaryError(JSContext* cx, Size package, StringView resolved, StringView specifier,
                        StringView referrerUrl)
{
    String text;
    text.Append(StringView("Cannot import \"")).Ignore();
    text.Append(specifier).Ignore();
    text.Append(StringView("\": ")).Ignore();
    text.Append(resolved).Ignore();
    text.Append(StringView(" is not part of ")).Ignore();
    AppendPackageLabel(text, package);
    text.Append(StringView(". A package's files can be imported from another package only by "
                           "its name, through its \"exports\".")).Ignore();
    return ThrowModuleError(cx, "PermissionDenied", text.View(), specifier, referrerUrl);
}

/// Phase 9.3: leaves `exception` (taken in another realm) pending in the
/// current one.
void RethrowHere(JSContext* cx, bool thrown, JS::HandleValue exception)
{
    if (!thrown)
    {
        return;  // an uncatchable error: nothing to rethrow
    }

    JS::RootedValue value(cx, exception);

    if (JS_WrapValue(cx, &value))
    {
        JS_SetPendingException(cx, value);
    }
}

/// Phase 9.3: the global object of the compartment that package `index`
/// runs in.
JSObject* PackageGlobal(JSContext* cx, Size index)
{
    return CompartmentGlobal(cx, g_registry->packages[index]->compartment);
}

/// Reads, compiles (in the package's compartment) and registers the module
/// at `url`, or returns the one already loaded. Throws (returns null) in
/// the current realm on failure.
ModuleRecord* LoadModule(JSContext* cx, StringView url, JS::ModuleType type, StringView specifier,
                         StringView referrerUrl, bool isEntry, Size package)
{
    if (ModuleRecord* known = Lookup(type, url))
    {
        if (known->package != package)
        {
            ThrowBoundaryError(cx, package, known->path.View(), specifier, referrerUrl);
            return nullptr;
        }

        return known;
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

    // Phase 9.2: the file must belong to `package`: inside its folder, with
    // no other package's folder in between.
    if (!InsidePackage(package, resolved))
    {
        ThrowBoundaryError(cx, package, resolved, specifier, referrerUrl);
        return nullptr;
    }

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

        return known;
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

    // Phase 9.3: compiled in the realm of the package's compartment, so
    // its code runs with that compartment's (frozen) intrinsics.
    JS::RootedObject global(cx, PackageGlobal(cx, package));

    if (!global)
    {
        return nullptr;
    }

    JS::RootedObject module(cx);
    JS::RootedValue exception(cx);
    bool thrown = false;

    {
        JSAutoRealm ar(cx, global);
        JS::CompileOptions options(cx);
        options.setFileAndLine(filename.CStr(), 1);

        JS::SourceText<mozilla::Utf8Unit> text;

        if (text.init(cx, length == 0 ? "" : source, length, JS::SourceOwnership::Borrowed))
        {
            module = type == JS::ModuleType::JSON ? JS::CompileJsonModule(cx, options, text)
                                                  : JS::CompileModule(cx, options, text);
        }

        if (!module)
        {
            thrown = TakePendingException(cx, &exception);
        }
    }

    if (!module)
    {
        RethrowHere(cx, thrown, exception);
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
    record->package = package;
    record->compartment = g_registry->packages[package]->compartment;
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

    return record;
}

StringView UrlOf(ModuleRecord* record)
{
    return record != nullptr ? record->url.View() : StringView();
}

/// Phase 9.2: reads and validates the forge.json of the package at `root`.
bool ReadManifest(JSContext* cx, StringView root, bool isApp, Manifest& manifest,
                  StringView specifier, StringView referrerUrl)
{
    forge::kavach::Denial denial;
    Result<forge::kavach::CheckedPath> checked = forge::kavach::CheckManifest(root, denial);

    if (checked.HasError())
    {
        ThrowKavachDenial(cx, Op::ModuleImport, denial);
        DefineStringOnPendingError(cx, "specifier", specifier);
        return false;
    }

    Result<Vector<u8>> bytes = forge::kavach::fs::ReadFile(checked.Value(), denial);

    if (bytes.HasError())
    {
        if (denial.kind != forge::kavach::Denial::Kind::None)
        {
            ThrowKavachDenial(cx, Op::ModuleImport, denial);
            DefineStringOnPendingError(cx, "specifier", specifier);
            return false;
        }

        String text;
        text.Append(StringView("Cannot read ")).Ignore();
        text.Append(checked.Value().Text()).Ignore();
        return ThrowModuleError(cx, ErrorCodeToString(bytes.Error().Code()), text.View(), specifier,
                                referrerUrl);
    }

    String message;
    const StringView data(reinterpret_cast<const char*>(bytes.Value().Data()), bytes.Value().Size());

    if (!ParseManifest(data, isApp, manifest, message))
    {
        String text;
        text.Append(checked.Value().Text()).Ignore();
        text.Append(StringView(" is not valid: ")).Ignore();
        text.Append(message.View()).Ignore();
        return ThrowModuleError(cx, "InvalidData", text.View(), specifier, referrerUrl);
    }

    return true;
}

/// Phase 9.2: registers a package. `root` must be canonical and resolved.
/// Phase 9.4: `compartment` is the dependency's (the app's is always
/// kAppCompartment).
bool AddPackage(JSContext* cx, StringView root, bool isApp, bool rootIsFile, bool hasManifest,
                Manifest&& manifest, Size& index, Size compartment = kPureCompartment)
{
    Result<UniquePtr<Package>> made = forge::core::memory::MakeUnique<Package>();

    if (made.HasError() || made.Value()->root.Append(root).HasError() ||
        !PathToFileUrl(root, made.Value()->rootUrl))
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    Package& package = *made.Value();
    package.isApp = isApp;
    // D25: the app's own compartment; for a dependency, the shared "pure"
    // one or (Phase 9.4) its own if it holds a permission.
    package.compartment = isApp ? kAppCompartment : compartment;
    package.rootIsFile = rootIsFile;
    package.hasManifest = hasManifest;
    package.manifest = std::move(manifest);

    index = g_registry->packages.Size();
    String key;

    if (key.Append(root).HasError() || g_registry->packages.PushBack(std::move(made.Value())).HasError() ||
        g_registry->packageByRoot.Insert(key, index).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

/// Phase 9.4: copies a forge.json permission set into Kavach's form.
bool CopyPermissions(const PermissionSet& from, forge::kavach::PermissionLists& to)
{
    const Vector<String>* sources[3] = {&from.read, &from.write, &from.env};
    Vector<String>* targets[3] = {&to.read, &to.write, &to.env};

    for (int list = 0; list < 3; ++list)
    {
        for (Size i = 0; i < sources[list]->Size(); ++i)
        {
            String copy;

            if (copy.Append((*sources[list])[i].View()).HasError() ||
                targets[list]->PushBack(std::move(copy)).HasError())
            {
                return false;
            }
        }
    }

    return true;
}

/// Phase 9.4: registers the policy of the dependency `manifest` (at
/// `root`): what it declares, intersected with what the program's
/// forge.json grants it, capped by the command line (D26). A package that
/// holds a permission gets a compartment of its own, with Forge's natives
/// bound to its policy (D24, D25); any other stays in the pure compartment.
bool SetUpPackagePolicy(JSContext* cx, const Manifest& manifest, StringView root,
                        StringView specifier, StringView referrerUrl, Size& compartment)
{
    compartment = kPureCompartment;
    const Package& app = *g_registry->packages[0];
    const PackageGrant* grant = nullptr;

    for (Size i = 0; app.hasManifest && i < app.manifest.grants.Size(); ++i)
    {
        if (app.manifest.grants[i].name.View() == manifest.name.View())
        {
            grant = &app.manifest.grants[i];
        }
    }

    if (manifest.permissions.Empty() && grant == nullptr)
    {
        return true;
    }

    // A grant names exactly one package. Until forge.lock binds a grant to
    // a package's identity (Phase 14), a second folder holding a package of
    // the same name is refused rather than given the same permissions.
    if (grant != nullptr)
    {
        for (Size i = 1; i < g_registry->packages.Size(); ++i)
        {
            const Package& other = *g_registry->packages[i];

            if (other.manifest.name.View() == manifest.name.View() &&
                !forge::kavach::PathEquals(other.root.View(), root))
            {
                String text;
                text.Append(StringView("The program's forge.json grants permissions to \"")).Ignore();
                text.Append(manifest.name.View()).Ignore();
                text.Append(StringView("\", but two folders hold a package of that name (")).Ignore();
                text.Append(other.root.View()).Ignore();
                text.Append(StringView(" and ")).Ignore();
                text.Append(root).Ignore();
                text.Append(StringView("). A grant must name exactly one package.")).Ignore();
                return ThrowModuleError(cx, "PermissionDenied", text.View(), specifier, referrerUrl);
            }
        }
    }

    forge::kavach::PermissionLists declared;
    forge::kavach::PermissionLists granted;

    if (!CopyPermissions(manifest.permissions, declared) ||
        (grant != nullptr && !CopyPermissions(grant->permissions, granted)))
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    const forge::kavach::PackagePolicy* policy = nullptr;
    String message;

    if (!forge::kavach::AddPackagePolicy(manifest.name.View(), app.root.View(), declared, granted,
                                         policy, message))
    {
        String text;
        text.Append(StringView("The permissions of the package \"")).Ignore();
        text.Append(manifest.name.View()).Ignore();
        text.Append(StringView("\" cannot be used: ")).Ignore();
        text.Append(message.View()).Ignore();
        return ThrowModuleError(cx, "InvalidData", text.View(), specifier, referrerUrl);
    }

    if (forge::kavach::HoldsPermission(policy))
    {
        if (!AddPackageCompartment(policy, compartment))
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }

        return true;
    }

    // Declared but not granted: the package runs without it. Say so once,
    // so a failure inside the package is not a mystery.
    String missing;
    forge::kavach::DescribeUngranted(policy, missing);

    if (!missing.Empty())
    {
        String line;
        line.Append(StringView("Forge: the package \"")).Ignore();
        line.Append(manifest.name.View()).Ignore();
        line.Append(StringView("\" runs without the permissions it declares (")).Ignore();
        line.Append(missing.View()).Ignore();
        line.Append(StringView("): the program's forge.json does not grant them.")).Ignore();
        WriteErrorLine(line.View());
    }

    return true;
}

/// Phase 9.2: loads the package that `dependency` of package `from` names,
/// or finds the one already loaded.
bool LoadDependency(JSContext* cx, Size from, const Dependency& dependency, StringView specifier,
                    StringView referrerUrl, Size& out)
{
    const Package& owner = *g_registry->packages[from];
    String resolved;
    String message;

    if (!forge::kavach::ResolvePackageFolder(dependency.path.View(), owner.root.View(), resolved,
                                              message))
    {
        String text;
        text.Append(StringView("Cannot load the package \"")).Ignore();
        text.Append(dependency.name.View()).Ignore();
        text.Append(StringView("\" listed in the dependencies of ")).Ignore();
        AppendPackageLabel(text, from);
        text.Append(StringView(": ")).Ignore();
        text.Append(message.View()).Ignore();
        return ThrowModuleError(cx, "NotFound", text.View(), specifier, referrerUrl);
    }

    // A dependency may only point inside the program's folder; only the
    // program's own forge.json may point elsewhere (PHASE9_DESIGN.md, 4).
    // Checked before the cache, so a folder the program itself named does
    // not become reachable from a dependency that names it too.
    const Package& app = *g_registry->packages[0];

    if (!owner.isApp &&
        (app.rootIsFile || !forge::kavach::PathCovers(app.root.View(), resolved.View())))
    {
        String text;
        text.Append(StringView("The package \"")).Ignore();
        text.Append(owner.manifest.name.View()).Ignore();
        text.Append(StringView("\" lists \"")).Ignore();
        text.Append(dependency.name.View()).Ignore();
        text.Append(StringView("\" at ")).Ignore();
        text.Append(resolved.View()).Ignore();
        text.Append(StringView(", outside the program's folder (")).Ignore();
        text.Append(app.root.View()).Ignore();
        text.Append(StringView("). Only the program's own forge.json may name a folder "
                               "outside it.")).Ignore();
        return ThrowModuleError(cx, "PermissionDenied", text.View(), specifier, referrerUrl);
    }

    if (const Size* known = g_registry->packageByRoot.Find(resolved))
    {
        const Package& package = *g_registry->packages[*known];

        if (package.isApp || !(package.manifest.name.View() == dependency.name.View()))
        {
            String text;
            text.Append(StringView("The dependency \"")).Ignore();
            text.Append(dependency.name.View()).Ignore();
            text.Append(StringView("\" points at ")).Ignore();
            AppendPackageLabel(text, *known);
            text.Append(StringView(", which is not a package of that name")).Ignore();
            return ThrowModuleError(cx, "InvalidData", text.View(), specifier, referrerUrl);
        }

        out = *known;
        return true;
    }

    if (!forge::kavach::AddModuleRoot(resolved.View(), message))
    {
        return ThrowModuleError(cx, "PermissionDenied", message.View(), specifier, referrerUrl);
    }

    Manifest manifest;

    if (!ReadManifest(cx, resolved.View(), false, manifest, specifier, referrerUrl))
    {
        return false;
    }

    if (!(manifest.name.View() == dependency.name.View()))
    {
        String text;
        text.Append(StringView("The folder ")).Ignore();
        text.Append(resolved.View()).Ignore();
        text.Append(StringView(" holds the package \"")).Ignore();
        text.Append(manifest.name.View()).Ignore();
        text.Append(StringView("\", not \"")).Ignore();
        text.Append(dependency.name.View()).Ignore();
        text.Append(StringView("\"")).Ignore();
        return ThrowModuleError(cx, "InvalidData", text.View(), specifier, referrerUrl);
    }

    Size compartment = kPureCompartment;

    if (!SetUpPackagePolicy(cx, manifest, resolved.View(), specifier, referrerUrl, compartment))
    {
        return false;
    }

    return AddPackage(cx, resolved.View(), false, false, true, std::move(manifest), out, compartment);
}

/// Resolves `specifier`, imported by a module of package `from` whose URL is
/// `referrerUrl`, to a module URL and the package that module must belong
/// to. Relative and file: specifiers stay in `from`; a bare specifier names
/// a declared dependency (or the package itself) and goes through its
/// "exports" (Phase 9.2).
bool ResolveToUrl(JSContext* cx, Size from, StringView referrerUrl, StringView specifier,
                  String& url, Size& package)
{
    const SpecifierError error = ResolveSpecifier(specifier, referrerUrl, url);

    if (error == SpecifierError::None)
    {
        package = from;
        return true;
    }

    if (error != SpecifierError::Bare)
    {
        String text;
        text.Append(StringView("Cannot import \"")).Ignore();
        text.Append(specifier).Ignore();
        text.Append(StringView("\" from ")).Ignore();
        AppendImporter(text, referrerUrl);
        text.Append(StringView(": ")).Ignore();
        text.Append(StringView(DescribeSpecifierError(error))).Ignore();
        return ThrowModuleError(cx, SpecifierErrorCode(error), text.View(), specifier, referrerUrl);
    }

    String name;
    String subpath;

    if (!SplitBareSpecifier(specifier, name, subpath))
    {
        String text;
        text.Append(StringView("Cannot import \"")).Ignore();
        text.Append(specifier).Ignore();
        text.Append(StringView("\": it is not a valid package name (lower-case letters, digits, "
                               "- . _ ~, optionally @scope/) followed by a subpath")).Ignore();
        return ThrowModuleError(cx, "InvalidArgument", text.View(), specifier, referrerUrl);
    }

    const Package& owner = *g_registry->packages[from];
    Size target = 0;

    if (owner.hasManifest && !owner.manifest.name.Empty() &&
        owner.manifest.name.View() == name.View())
    {
        target = from;  // a package importing itself by name, through its own exports
    }
    else
    {
        const Dependency* dependency = nullptr;

        for (Size i = 0; owner.hasManifest && i < owner.manifest.dependencies.Size(); ++i)
        {
            if (owner.manifest.dependencies[i].name.View() == name.View())
            {
                dependency = &owner.manifest.dependencies[i];
            }
        }

        if (dependency == nullptr)
        {
            String text;
            text.Append(StringView("Cannot import the package \"")).Ignore();
            text.Append(name.View()).Ignore();
            text.Append(StringView("\": ")).Ignore();

            if (owner.hasManifest)
            {
                text.Append(StringView("it is not listed in \"dependencies\" in ")).Ignore();
                text.Append(owner.root.View()).Ignore();
                text.Append(StringView("\\forge.json")).Ignore();
            }
            else
            {
                text.Append(StringView("the program has no forge.json; add one next to it with "
                                       "\"dependencies\"")).Ignore();
            }

            return ThrowModuleError(cx, "NotFound", text.View(), specifier, referrerUrl);
        }

        if (!LoadDependency(cx, from, *dependency, specifier, referrerUrl, target))
        {
            return false;
        }
    }

    const Package& pkg = *g_registry->packages[target];
    String exportTarget;
    const ExportResult result = ResolveExport(pkg.manifest, subpath.View(), exportTarget);

    if (result == ExportResult::Found)
    {
        String base;

        if (base.Append(pkg.rootUrl.View()).HasError() || base.Append('/').HasError() ||
            base.Append(StringView(kManifestName)).HasError())
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }

        const SpecifierError targetError = ResolveSpecifier(exportTarget.View(), base.View(), url);

        if (targetError != SpecifierError::None)
        {
            return ThrowModuleError(cx, SpecifierErrorCode(targetError),
                                    StringView(DescribeSpecifierError(targetError)), specifier,
                                    referrerUrl);
        }

        package = target;
        return true;
    }

    String text;
    text.Append(StringView("Cannot import \"")).Ignore();
    text.Append(specifier).Ignore();
    text.Append(StringView("\": the package \"")).Ignore();
    text.Append(name.View()).Ignore();

    switch (result)
    {
    case ExportResult::NoExports:
        text.Append(StringView("\" has no \"exports\" in its forge.json, so nothing in it can be "
                               "imported")).Ignore();
        break;
    case ExportResult::Blocked:
        text.Append(StringView("\" marks \"")).Ignore();
        text.Append(subpath.View()).Ignore();
        text.Append(StringView("\" as private (null in \"exports\")")).Ignore();
        break;
    case ExportResult::Invalid:
        text.Append(StringView("\" maps \"")).Ignore();
        text.Append(subpath.View()).Ignore();
        text.Append(StringView("\" to a path that would leave the package")).Ignore();
        return ThrowModuleError(cx, "InvalidArgument", text.View(), specifier, referrerUrl);
    default:
        text.Append(StringView("\" does not export \"")).Ignore();
        text.Append(subpath.View()).Ignore();
        text.Append('"').Ignore();
        break;
    }

    return ThrowModuleError(cx, "PermissionDenied", text.View(), specifier, referrerUrl);
}

bool OnLoadResolved(JSContext* cx, JS::HandleValue hostDefined);
bool OnLoadRejected(JSContext* cx, JS::HandleValue hostDefined, JS::HandleValue error);
double RecordNumber(ModuleRecord* record);
ModuleRecord* RecordFromNumber(const JS::Value& value);

Size IndexOfRecord(const ModuleRecord* record)
{
    Size index = 0;

    while (g_registry->records[index].Get() != record)
    {
        ++index;
    }

    return index;
}

/// Phase 9.3: appends `name` as the inside of a JavaScript string literal:
/// letters, digits, `_` and `$` as they are, everything else as \uXXXX.
/// Export names are well-formed Unicode (the language requires it), so
/// the result is always valid source.
bool AppendEscapedName(String& out, StringView name)
{
    const unsigned char* p = reinterpret_cast<const unsigned char*>(name.Data());
    const unsigned char* end = p + name.Size();

    auto unit = [&out](unsigned value) -> bool
    {
        char escape[8];
        snprintf(escape, sizeof(escape), "\\u%04X", value & 0xFFFFu);
        return out.Append(StringView(escape)).HasValue();
    };

    while (p < end)
    {
        const unsigned char c = *p;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '$')
        {
            if (out.Append(static_cast<char>(c)).HasError())
            {
                return false;
            }

            ++p;
            continue;
        }

        unsigned codePoint = 0xFFFD;
        Size length = 1;

        if (c < 0x80)
        {
            codePoint = c;
        }
        else if ((c & 0xE0) == 0xC0 && end - p >= 2)
        {
            codePoint = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
            length = 2;
        }
        else if ((c & 0xF0) == 0xE0 && end - p >= 3)
        {
            codePoint = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            length = 3;
        }
        else if ((c & 0xF8) == 0xF0 && end - p >= 4)
        {
            codePoint = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) |
                        (p[3] & 0x3Fu);
            length = 4;
        }

        p += length;

        if (codePoint >= 0x10000)
        {
            codePoint -= 0x10000;

            if (!unit(0xD800 + (codePoint >> 10)) || !unit(0xDC00 + (codePoint & 0x3FF)))
            {
                return false;
            }
        }
        else if (!unit(codePoint))
        {
            return false;
        }
    }

    return true;
}

/// Phase 9.3: the source of a facade module. It awaits the target package's
/// evaluation through import.meta.load() (a Forge function only this module
/// can reach), then re-exports a snapshot of every export, read from
/// import.meta.namespace. It is generated from the export names alone,
/// never from the package's source text. (The namespace is not the
/// promise's value: a package exporting `then` would make it a thenable.)
bool BuildFacadeSource(const Vector<String>& names, String& source)
{
    bool ok = source.Append(StringView("await import.meta.load();\n"
                                       "const n = import.meta.namespace;\n")).HasValue();

    for (Size i = 0; ok && i < names.Size(); ++i)
    {
        char binding[48];
        snprintf(binding, sizeof(binding), "const e%zu = n[\"", static_cast<size_t>(i));
        ok = source.Append(StringView(binding)).HasValue() &&
             AppendEscapedName(source, names[i].View()) &&
             source.Append(StringView("\"];\n")).HasValue();
    }

    ok = ok && source.Append(StringView("export {")).HasValue();

    for (Size i = 0; ok && i < names.Size(); ++i)
    {
        char binding[48];
        snprintf(binding, sizeof(binding), "%s e%zu as \"", i == 0 ? "" : ",",
                 static_cast<size_t>(i));
        ok = source.Append(StringView(binding)).HasValue() &&
             AppendEscapedName(source, names[i].View()) && source.Append('"').HasValue();
    }

    return ok && source.Append(StringView(" };\n")).HasValue();
}

/// Phase 9.3: "greet -> util -> greet", the packages of an import cycle.
void AppendCycle(String& text, Size repeated)
{
    bool started = false;

    for (Size i = 0; i < g_registry->loadingPackages.Size(); ++i)
    {
        const Size index = g_registry->loadingPackages[i];

        if (!started && index != repeated)
        {
            continue;
        }

        started = true;
        const Package& package = *g_registry->packages[index];
        text.Append(package.isApp ? StringView("the program") : package.manifest.name.View()).Ignore();
        text.Append(StringView(" -> ")).Ignore();
    }

    text.Append(g_registry->packages[repeated]->manifest.name.View()).Ignore();
}

/// Phase 9.3: the facade through which modules of `compartment` import
/// `target`, a module of another package. The target's module graph is
/// loaded and linked in its own compartment first (an import cycle between
/// packages is refused here); the facade is compiled in `compartment`.
ModuleRecord* GetFacade(JSContext* cx, ModuleRecord* target, Size compartment,
                        StringView specifier, StringView referrerUrl)
{
    const Size targetIndex = IndexOfRecord(target);
    char keyText[64];
    snprintf(keyText, sizeof(keyText), "F|%zu|%zu", static_cast<size_t>(targetIndex),
             static_cast<size_t>(compartment));
    String key;

    if (key.Append(StringView(keyText)).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    if (const Size* known = g_registry->byKey.Find(key))
    {
        return g_registry->records[*known].Get();
    }

    for (Size i = 0; i < g_registry->loadingPackages.Size(); ++i)
    {
        if (g_registry->loadingPackages[i] == target->package)
        {
            String text;
            text.Append(StringView("Cannot import \"")).Ignore();
            text.Append(specifier).Ignore();
            text.Append(StringView("\": it closes an import cycle between packages (")).Ignore();
            AppendCycle(text, target->package);
            text.Append(StringView("). Import cycles are allowed only inside one package.")).Ignore();
            ThrowModuleError(cx, "InvalidArgument", text.View(), specifier, referrerUrl);
            return nullptr;
        }
    }

    JS::RootedObject targetGlobal(cx, CompartmentGlobal(cx, target->compartment));
    JS::RootedObject facadeGlobal(cx, targetGlobal ? CompartmentGlobal(cx, compartment) : nullptr);

    if (!facadeGlobal || g_registry->loadingPackages.PushBack(target->package).HasError())
    {
        if (!JS_IsExceptionPending(cx))
        {
            JS_ReportOutOfMemory(cx);
        }

        return nullptr;
    }

    // Load and link the target's graph, then read its export names. The
    // namespace's keys are its exports; reading them runs no code and does
    // not touch the (not yet initialized) bindings.
    Vector<String> names;
    JS::RootedValue exception(cx);
    bool thrown = false;
    bool ok = false;

    {
        JSAutoRealm ar(cx, targetGlobal);
        JS::RootedObject module(cx, target->module);
        JS::RootedValue hostDefined(cx, JS::ObjectValue(*module));
        ok = JS::LoadRequestedModules(cx, module, hostDefined, OnLoadResolved, OnLoadRejected) &&
             !JS_IsExceptionPending(cx);

        JS::RootedObject ns(cx, ok ? JS::GetModuleNamespace(cx, module) : nullptr);
        JS::RootedIdVector ids(cx);
        ok = ns && js::GetPropertyKeys(cx, ns, JSITER_OWNONLY | JSITER_HIDDEN, &ids);

        for (size_t i = 0; ok && i < ids.length(); ++i)
        {
            if (!ids[i].isString())
            {
                continue;
            }

            String name;
            ok = AppendStringUtf8(cx, ids[i].toString(), name) &&
                 names.PushBack(std::move(name)).HasValue();
        }

        if (!ok)
        {
            if (!JS_IsExceptionPending(cx))
            {
                JS_ReportOutOfMemory(cx);
            }

            thrown = TakePendingException(cx, &exception);
        }
    }

    g_registry->loadingPackages.PopBack();

    if (!ok)
    {
        RethrowHere(cx, thrown, exception);
        return nullptr;
    }

    String source;
    String filename;

    if (!BuildFacadeSource(names, source) || filename.Append(target->path.View()).HasError() ||
        filename.Append(StringView(" (package facade)")).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    JS::RootedObject facade(cx);

    {
        JSAutoRealm ar(cx, facadeGlobal);
        JS::CompileOptions options(cx);
        options.setFileAndLine(filename.CStr(), 1);
        JS::SourceText<mozilla::Utf8Unit> text;

        if (text.init(cx, source.Data(), source.Size(), JS::SourceOwnership::Borrowed))
        {
            facade = JS::CompileModule(cx, options, text);
        }

        if (!facade)
        {
            thrown = TakePendingException(cx, &exception);
        }
    }

    if (!facade)
    {
        RethrowHere(cx, thrown, exception);
        return nullptr;
    }

    Result<UniquePtr<ModuleRecord>> made = forge::core::memory::MakeUnique<ModuleRecord>();

    if (made.HasError() || made.Value()->url.Append(target->url.View()).HasError() ||
        made.Value()->path.Append(target->path.View()).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    ModuleRecord* record = made.Value().Get();
    record->type = JS::ModuleType::JavaScript;
    record->package = target->package;
    record->compartment = compartment;
    record->isFacade = true;
    record->facadeTarget = targetIndex;
    record->module = facade;

    const Size index = g_registry->records.Size();

    if (g_registry->records.PushBack(std::move(made.Value())).HasError() ||
        g_registry->byKey.Insert(key, index).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    JS::SetModulePrivate(facade, JS::PrivateValue(record));
    return record;
}

/// Phase 9.3: import.meta.load() of a facade (`this` is the facade's
/// import.meta). Evaluates the target module in its own compartment (once;
/// later calls share that evaluation), defines import.meta.namespace (the
/// target's namespace, wrapped for the facade's compartment) and returns a
/// promise that settles when the evaluation does. Reserved slot 0 holds the
/// facade's record number.
bool FacadeLoad(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    ModuleRecord* facade = RecordFromNumber(js::GetFunctionNativeReserved(&args.callee(), 0));

    if (facade == nullptr || !facade->isFacade || !args.thisv().isObject())
    {
        JS_ReportErrorASCII(cx, "import.meta.load: not a package facade");
        return false;
    }

    JS::RootedObject meta(cx, &args.thisv().toObject());

    ModuleRecord* target = g_registry->records[facade->facadeTarget].Get();
    JS::RootedObject targetGlobal(cx, CompartmentGlobal(cx, target->compartment));

    if (!targetGlobal)
    {
        return false;
    }

    JS::RootedValue evaluation(cx);
    JS::RootedValue ns(cx);
    JS::RootedValue exception(cx);
    bool thrown = false;
    bool ok = false;

    {
        JSAutoRealm ar(cx, targetGlobal);
        JS::RootedObject module(cx, target->module);
        ok = JS::ModuleEvaluate(cx, module, &evaluation);

        JSObject* nsObject = ok ? JS::GetModuleNamespace(cx, module) : nullptr;
        ok = nsObject != nullptr;

        if (ok)
        {
            ns.setObject(*nsObject);
        }
        else
        {
            thrown = TakePendingException(cx, &exception);
        }
    }

    if (!ok)
    {
        RethrowHere(cx, thrown, exception);
        return false;
    }

    if (!JS_WrapValue(cx, &evaluation) || !JS_WrapValue(cx, &ns) ||
        !JS_DefineProperty(cx, meta, "namespace", ns, JSPROP_READONLY | JSPROP_PERMANENT))
    {
        return false;
    }

    // A promise of this realm that settles like the evaluation (fulfilled
    // with undefined, or rejected with the package's error).
    JS::RootedObject promise(cx, evaluation.isObject() ? &evaluation.toObject() : nullptr);
    JS::RootedObject result(cx, promise ? JS::CallOriginalPromiseThen(cx, promise, nullptr, nullptr)
                                        : JS::CallOriginalPromiseResolve(cx, JS::UndefinedHandleValue));

    if (!result)
    {
        return false;
    }

    args.rval().setObject(*result);
    return true;
}

/// Resolves `moduleRequest` for `referrer` (null for a classic script) and
/// loads it.
JSObject* ResolveAndLoad(JSContext* cx, ModuleRecord* referrer, JS::HandleObject moduleRequest)
{
    const StringView referrerUrl = UrlOf(referrer);
    JS::RootedString specifierString(cx, JS::GetModuleRequestSpecifier(cx, moduleRequest));
    String specifier;

    if (!specifierString || !AppendStringUtf8(cx, specifierString, specifier))
    {
        return nullptr;
    }

    if (g_registry->packages.Empty())
    {
        ThrowModuleError(cx, "NotSupported",
                         StringView("Modules can be imported only by a program started as "
                                    "`forge file.js`"),
                         specifier.View(), referrerUrl);
        return nullptr;
    }

    if (JS::ModuleRequestIsSourcePhase(cx, moduleRequest))
    {
        ThrowModuleError(cx, "NotSupported", StringView("Source-phase imports are not supported"),
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

    const Size from = referrer != nullptr ? referrer->package : 0;
    String url;
    Size package = 0;

    if (!ResolveToUrl(cx, from, referrerUrl, specifier.View(), url, package))
    {
        return nullptr;
    }

    ModuleRecord* record =
        LoadModule(cx, url.View(), type, specifier.View(), referrerUrl, false, package);

    if (record == nullptr)
    {
        return nullptr;
    }

    if (package == from)
    {
        return record->module;
    }

    // Phase 9.3: another package runs in another compartment (or is its
    // own module graph in the shared one); it is reached through a facade.
    ModuleRecord* facade = GetFacade(cx, record, g_registry->packages[from]->compartment,
                                     specifier.View(), referrerUrl);
    return facade != nullptr ? facade->module : nullptr;
}

/// The index + 1 of `record` (0 for none), as stored where script cannot
/// reach it (the dynamic-import holder, import.meta.resolve's slot).
double RecordNumber(ModuleRecord* record)
{
    for (Size i = 0; record != nullptr && i < g_registry->records.Size(); ++i)
    {
        if (g_registry->records[i].Get() == record)
        {
            return static_cast<double>(i + 1);
        }
    }

    return 0;
}

ModuleRecord* RecordFromNumber(const JS::Value& value)
{
    if (!value.isNumber())
    {
        return nullptr;
    }

    const double n = value.toNumber();

    if (!(n >= 1) || n > static_cast<double>(g_registry->records.Size()))
    {
        return nullptr;
    }

    return g_registry->records[static_cast<Size>(n) - 1].Get();
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
/// [moduleRequest, payload, referrer record number (RecordNumber)], made by
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
    JS::RootedObject module(cx, ResolveAndLoad(cx, RecordFromNumber(referrerValue), moduleRequest));

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

    ModuleRecord* referrerRecord = RecordOf(referrer);

    JS::RootedObject payloadObject(cx, payload.isObject() ? &payload.toObject() : nullptr);

    if (payloadObject && JS::IsPromiseObject(payloadObject))
    {
        // import(): finish in a promise job, so it is always asynchronous
        // (as in the engine's shell).
        JS::RootedValueArray<3> items(cx);
        items[0].setObject(*moduleRequest);
        items[1].set(payload);
        items[2].setNumber(RecordNumber(referrerRecord));

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

    JS::RootedObject module(cx, ResolveAndLoad(cx, referrerRecord, moduleRequest));

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

    // Slot 0 holds the module's record number (script cannot read it).
    ModuleRecord* record = RecordFromNumber(js::GetFunctionNativeReserved(&args.callee(), 0));
    String specifier;

    if (record == nullptr || !AppendStringUtf8(cx, args[0].toString(), specifier))
    {
        if (!JS_IsExceptionPending(cx))
        {
            JS_ReportErrorASCII(cx, "import.meta.resolve: the module is not known");
        }

        return false;
    }

    // Packages are resolved too (their manifests may be read); nothing is
    // loaded or evaluated.
    String url;
    Size package = 0;

    if (!ResolveToUrl(cx, record->package, record->url.View(), specifier.View(), url, package))
    {
        return false;
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

    if (record->isFacade)
    {
        // Phase 9.3: only the facade's own (generated) code sees this.
        JSFunction* load = js::DefineFunctionWithReserved(cx, metaObject, "load", FacadeLoad, 0, 0);

        if (!load)
        {
            return false;
        }

        JS::RootedObject loadObject(cx, JS_GetFunctionObject(load));
        js::SetFunctionNativeReserved(loadObject, 0, JS::NumberValue(RecordNumber(record)));
        return MarkHostObject(cx, loadObject);
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

    JS::RootedObject resolveObject(cx, JS_GetFunctionObject(resolve));
    js::SetFunctionNativeReserved(resolveObject, 0, JS::NumberValue(RecordNumber(record)));
    // Phase 9.3: it resolves with this module's package; it must not
    // reach another package.
    return MarkHostObject(cx, resolveObject);
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

EntryResult RunEntryModule(JSContext* cx, StringView entryPath, bool frozenIntrinsics)
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

    // Phase 9.2: the program's own package. Its root is the module root
    // Kavach chose (the folder with forge.json, the entry's folder, or the
    // entry file alone).
    const StringView root = forge::kavach::FirstModuleRoot();
    const bool rootIsFile = forge::kavach::PathEquals(root, entryPath);
    bool hasManifest = false;
    Manifest manifest;

    if (!rootIsFile)
    {
        String resolved;
        String unused;
        hasManifest = forge::kavach::ResolvePackageFolder(root, root, resolved, unused);

        if (hasManifest && !ReadManifest(cx, root, true, manifest, url.View(), StringView()))
        {
            return EntryResult::Threw;
        }
    }

    // Phase 9.3: a program with dependencies has its intrinsics recorded
    // now, before any code runs, so none of them can ever reach a package
    // as an ordinary wrapper.
    JS::RootedObject global(cx, JS::CurrentGlobalOrNull(cx));
    const bool hasDependencies = hasManifest && !manifest.dependencies.Empty();

    if (!global || !PrepareAppRealm(cx, global, hasDependencies, frozenIntrinsics))
    {
        return EntryResult::Threw;
    }

    Size appPackage = 0;

    if (!AddPackage(cx, root, true, rootIsFile, hasManifest, std::move(manifest), appPackage))
    {
        return EntryResult::Threw;
    }

    ModuleRecord* entry = LoadModule(cx, url.View(), JS::ModuleType::JavaScript, url.View(),
                                     StringView(), true, appPackage);

    if (entry == nullptr)
    {
        return EntryResult::Threw;
    }

    JS::RootedObject module(cx, entry->module);

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
