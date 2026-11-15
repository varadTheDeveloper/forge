// Compartments (Phase 9.3). See Compartments.h and PHASE9_DESIGN.md section 5.

#include "runtime/Compartments.h"

#include <utility>

#include "js/Array.h"
#include "js/CallArgs.h"
#include "js/CompilationAndEvaluation.h"
#include "js/CompileOptions.h"
#include "js/GlobalObject.h"
#include "js/Object.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"
#include "js/Realm.h"
#include "js/SourceText.h"
#include "js/Symbol.h"
#include "js/TracingAPI.h"
#include "js/WeakMap.h"
#include "js/Wrapper.h"
#include "js/WrapperCallbacks.h"
#include "jsapi.h"
#include "jsfriendapi.h"

#include "forge-core/memory/MakeUnique.h"
#include "forge-core/memory/UniquePtr.h"
#include "kavach/Kavach.h"
#include "runtime/Inspect.h"
#include "runtime/JsUtil.h"

namespace forge::runtime
{

namespace
{

using forge::core::Result;
using forge::core::Size;
using forge::core::String;
using forge::core::StringView;
using forge::core::u32;
using forge::core::Vector;
using forge::core::memory::UniquePtr;

enum class RealmKind
{
    App,
    Dependency,
};

/// Kept in the realm's private slot (JS::SetRealmPrivate). Phase 9.4: a
/// package compartment's record exists (with its policy) before its global
/// is created on first use.
struct RealmInfo
{
    RealmKind kind = RealmKind::App;
    /// Phase 9.4 (D24): the authority of every Forge native created in this
    /// realm. Null for the program; NoPermissionPolicy for the pure
    /// compartment; the package's policy for a package compartment.
    const forge::kavach::PackagePolicy* policy = nullptr;
    bool walked = false;  // `kinds` and `intrinsics` are filled in
    bool frozen = false;  // every intrinsic is frozen
    JS::Heap<JSObject*> global;
    /// WeakMap: object -> int32. >= 0: the index of an intrinsic in
    /// `intrinsics`; kForgeObject: one of Forge's own objects.
    JS::Heap<JSObject*> kinds;
    /// Array of the intrinsics, in walk order.
    JS::Heap<JSObject*> intrinsics;
    /// One per intrinsic: the class name, the number of own keys and
    /// whether it is callable, hashed. Two realms agree on an intrinsic
    /// only if their signatures match.
    Vector<u32> signatures;
};

constexpr int32_t kForgeObject = -1;
constexpr int32_t kUnknown = -2;

struct State
{
    CompartmentHooks hooks;
    Vector<String> standardNames;
    /// Index = compartment number: kAppCompartment, kPureCompartment, then
    /// one per permission-holding package (Phase 9.4). A slot may be empty
    /// until its compartment is set up.
    Vector<UniquePtr<RealmInfo>> realms;
    bool tracing = false;
};

State* g_state = nullptr;

void TraceCompartments(JSTracer* trc, void* data)
{
    (void)data;

    if (g_state == nullptr)
    {
        return;
    }

    for (Size i = 0; i < g_state->realms.Size(); ++i)
    {
        RealmInfo* info = g_state->realms[i].Get();

        if (info == nullptr)
        {
            continue;
        }

        if (info->global.unbarrieredGet() != nullptr)
        {
            JS::TraceEdge(trc, &info->global, "forge-compartment-global");
        }

        if (info->kinds.unbarrieredGet() != nullptr)
        {
            JS::TraceEdge(trc, &info->kinds, "forge-compartment-kinds");
        }

        if (info->intrinsics.unbarrieredGet() != nullptr)
        {
            JS::TraceEdge(trc, &info->intrinsics, "forge-compartment-intrinsics");
        }
    }
}

RealmInfo* InfoOf(JS::Realm* realm)
{
    return realm != nullptr ? static_cast<RealmInfo*>(JS::GetRealmPrivate(realm)) : nullptr;
}

RealmInfo* InfoOfObject(JSObject* obj)
{
    return InfoOf(js::GetNonCCWObjectRealm(obj));
}

/// The kind recorded for `obj` in `info` (kUnknown if none). Call inside
/// obj's realm.
bool KindOf(JSContext* cx, const RealmInfo& info, JS::HandleObject obj, int32_t* kind)
{
    *kind = kUnknown;

    if (info.kinds.unbarrieredGet() == nullptr)
    {
        return true;
    }

    JS::RootedObject map(cx, info.kinds);
    JS::RootedValue key(cx, JS::ObjectValue(*obj));
    JS::RootedValue value(cx);

    if (!JS::GetWeakMapEntry(cx, map, key, &value))
    {
        return false;
    }

    if (value.isInt32())
    {
        *kind = value.toInt32();
    }

    return true;
}

bool SetKind(JSContext* cx, const RealmInfo& info, JS::HandleObject obj, int32_t kind)
{
    JS::RootedObject map(cx, info.kinds);
    JS::RootedValue key(cx, JS::ObjectValue(*obj));
    JS::RootedValue value(cx, JS::Int32Value(kind));
    return JS::SetWeakMapEntry(cx, map, key, value);
}

int CompareBytes(StringView a, StringView b)
{
    const Size n = a.Size() < b.Size() ? a.Size() : b.Size();

    for (Size i = 0; i < n; ++i)
    {
        const unsigned char x = static_cast<unsigned char>(a.Data()[i]);
        const unsigned char y = static_cast<unsigned char>(b.Data()[i]);

        if (x != y)
        {
            return x < y ? -1 : 1;
        }
    }

    return a.Size() == b.Size() ? 0 : (a.Size() < b.Size() ? -1 : 1);
}

/// A sort key for a property key, so two realms visit properties in the
/// same order whatever order they were created in: indexes first (numeric
/// order), then names, then symbols.
bool AppendSortKey(JSContext* cx, JS::HandleId id, String& out)
{
    if (id.isInt())
    {
        char digits[16];
        snprintf(digits, sizeof(digits), "0%010d", id.toInt());
        return out.Append(StringView(digits)).HasValue();
    }

    if (id.isString())
    {
        return out.Append('1').HasValue() && AppendStringUtf8(cx, id.toString(), out);
    }

    if (id.isSymbol())
    {
        JS::RootedSymbol symbol(cx, id.toSymbol());
        char code[16];
        snprintf(code, sizeof(code), "2%010u", static_cast<unsigned>(JS::GetSymbolCode(symbol)));

        if (out.Append(StringView(code)).HasError())
        {
            return false;
        }

        JSString* description = JS::GetSymbolDescription(cx, symbol);
        return description == nullptr || AppendStringUtf8(cx, description, out);
    }

    return out.Append('3').HasValue();
}

/// The own keys of `obj` in sort-key order.
bool SortedOwnKeys(JSContext* cx, JS::HandleObject obj, JS::MutableHandleIdVector sorted)
{
    JS::RootedIdVector ids(cx);

    if (!js::GetPropertyKeys(cx, obj, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &ids))
    {
        return false;
    }

    Vector<String> keys;
    Vector<Size> order;

    for (size_t i = 0; i < ids.length(); ++i)
    {
        String key;
        JS::RootedId id(cx, ids[i]);

        if (!AppendSortKey(cx, id, key) || keys.PushBack(std::move(key)).HasError() ||
            order.PushBack(static_cast<Size>(i)).HasError())
        {
            if (!JS_IsExceptionPending(cx))
            {
                JS_ReportOutOfMemory(cx);
            }

            return false;
        }
    }

    // Insertion sort: built-in objects have few keys.
    for (Size i = 1; i < order.Size(); ++i)
    {
        const Size current = order[i];
        Size j = i;

        while (j > 0 && CompareBytes(keys[order[j - 1]].View(), keys[current].View()) > 0)
        {
            order[j] = order[j - 1];
            --j;
        }

        order[j] = current;
    }

    for (Size i = 0; i < order.Size(); ++i)
    {
        if (!sorted.append(ids[order[i]]))
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }
    }

    return true;
}

u32 Signature(JSObject* obj, Size keyCount)
{
    u32 hash = 2166136261u;

    for (const char* c = JS::GetClass(obj)->name; *c != '\0'; ++c)
    {
        hash = (hash ^ static_cast<unsigned char>(*c)) * 16777619u;
    }

    hash ^= static_cast<u32>(keyCount) * 0x9E3779B9u;
    return JS::IsCallable(obj) ? hash ^ 1u : hash;
}

/// Hidden intrinsics that no property path from the global reaches. Run in
/// the realm being walked, before any program or package code. Every entry
/// keeps its position (undefined when a feature is missing), so two realms
/// always line up.
constexpr const char kHiddenIntrinsics[] =
    "(function () {\n"
    "  'use strict';\n"
    "  const out = [];\n"
    "  const add = (make) => { try { out.push(make()); } catch (e) { out.push(undefined); } };\n"
    "  const proto = Object.getPrototypeOf;\n"
    "  add(() => proto([][Symbol.iterator]()));\n"
    "  add(() => proto(''[Symbol.iterator]()));\n"
    "  add(() => proto(new Map()[Symbol.iterator]()));\n"
    "  add(() => proto(new Set()[Symbol.iterator]()));\n"
    "  add(() => proto(/a/[Symbol.matchAll]('')));\n"
    "  add(() => proto(function* () {}));\n"
    "  add(() => proto(async function () {}));\n"
    "  add(() => proto(async function* () {}));\n"
    "  add(() => proto(Iterator.from([]).map((x) => x)));\n"
    "  add(() => proto(Iterator.from({ next() { return { done: true }; } })));\n"
    "  add(() => proto(new Intl.Segmenter().segment('')));\n"
    "  add(() => proto(new Intl.Segmenter().segment('')[Symbol.iterator]()));\n"
    "  return out;\n"
    "})()\n";

/// Records every intrinsic of the current realm (whose global is `global`):
/// everything reachable from the standard globals and the hidden
/// intrinsics through prototypes, property values, getters and setters.
bool Walk(JSContext* cx, RealmInfo& info, JS::HandleObject global)
{
    JS::RootedObject kinds(cx, JS::NewWeakMapObject(cx));

    if (!kinds)
    {
        return false;
    }

    info.kinds = kinds;
    JS::RootedObjectVector order(cx);

    // Adds `value` to the walk if it is an object not seen yet.
    auto visit = [&](JS::HandleValue value) -> bool
    {
        if (!value.isObject())
        {
            return true;
        }

        JS::RootedObject obj(cx, &value.toObject());

        if (JS_IsGlobalObject(obj))
        {
            return true;
        }

        int32_t kind = kUnknown;

        if (!KindOf(cx, info, obj, &kind))
        {
            return false;
        }

        if (kind != kUnknown)
        {
            return true;
        }

        if (!SetKind(cx, info, obj, static_cast<int32_t>(order.length())) || !order.append(obj))
        {
            if (!JS_IsExceptionPending(cx))
            {
                JS_ReportOutOfMemory(cx);
            }

            return false;
        }

        return true;
    };

    // Visits a property's value, or its getter and setter.
    auto visitProperty = [&](JS::HandleObject obj, JS::HandleId id) -> bool
    {
        JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx);

        if (!JS_GetOwnPropertyDescriptorById(cx, obj, id, &desc))
        {
            return false;
        }

        if (desc.isNothing())
        {
            return true;
        }

        JS::RootedValue item(cx);

        if (desc->hasValue())
        {
            item = desc->value();

            if (!visit(item))
            {
                return false;
            }
        }

        if (desc->hasGetter() && desc->getter() != nullptr)
        {
            item.setObject(*desc->getter());

            if (!visit(item))
            {
                return false;
            }
        }

        if (desc->hasSetter() && desc->setter() != nullptr)
        {
            item.setObject(*desc->setter());

            if (!visit(item))
            {
                return false;
            }
        }

        return true;
    };

    // 1. The standard globals, in name order.
    Vector<Size> names;

    for (Size i = 0; i < g_state->standardNames.Size(); ++i)
    {
        Size j = names.Size();

        if (names.PushBack(i).HasError())
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }

        while (j > 0 && CompareBytes(g_state->standardNames[names[j - 1]].View(),
                                     g_state->standardNames[i].View()) > 0)
        {
            names[j] = names[j - 1];
            --j;
        }

        names[j] = i;
    }

    for (Size i = 0; i < names.Size(); ++i)
    {
        const String& name = g_state->standardNames[names[i]];
        JS::RootedString atom(cx, JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(name.Data(), name.Size())));
        JS::RootedId id(cx);

        if (!atom || !JS_StringToId(cx, atom, &id) || !visitProperty(global, id))
        {
            return false;
        }
    }

    // 2. The hidden intrinsics.
    {
        JS::CompileOptions options(cx);
        options.setFileAndLine("forge:lockdown", 1);
        JS::SourceText<mozilla::Utf8Unit> source;
        JS::RootedValue hidden(cx);

        if (!source.init(cx, kHiddenIntrinsics, sizeof(kHiddenIntrinsics) - 1,
                         JS::SourceOwnership::Borrowed) ||
            !JS::Evaluate(cx, options, source, &hidden) || !hidden.isObject())
        {
            if (!JS_IsExceptionPending(cx))
            {
                JS_ReportErrorASCII(cx, "Forge: the lockdown walk could not list hidden intrinsics");
            }

            return false;
        }

        JS::RootedObject list(cx, &hidden.toObject());
        uint32_t length = 0;

        if (!JS::GetArrayLength(cx, list, &length))
        {
            return false;
        }

        for (uint32_t i = 0; i < length; ++i)
        {
            JS::RootedValue item(cx);

            if (!JS_GetElement(cx, list, i, &item) || !visit(item))
            {
                return false;
            }
        }
    }

    // 3. Breadth first through prototypes and properties.
    for (size_t i = 0; i < order.length(); ++i)
    {
        JS::RootedObject obj(cx, order[i]);
        JS::RootedObject proto(cx);

        if (!JS_GetPrototype(cx, obj, &proto))
        {
            return false;
        }

        JS::RootedValue protoValue(cx, proto ? JS::ObjectValue(*proto) : JS::NullValue());

        if (!visit(protoValue))
        {
            return false;
        }

        JS::RootedIdVector keys(cx);

        if (!SortedOwnKeys(cx, obj, &keys))
        {
            return false;
        }

        for (size_t k = 0; k < keys.length(); ++k)
        {
            JS::RootedId id(cx, keys[k]);

            if (!visitProperty(obj, id))
            {
                return false;
            }
        }

        if (info.signatures.PushBack(Signature(obj, static_cast<Size>(keys.length()))).HasError())
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }
    }

    JS::RootedValueVector values(cx);

    for (size_t i = 0; i < order.length(); ++i)
    {
        if (!values.append(JS::ObjectValue(*order[i])))
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }
    }

    JS::RootedObject intrinsics(cx, JS::NewArrayObject(cx, values));

    if (!intrinsics)
    {
        return false;
    }

    info.intrinsics = intrinsics;
    info.walked = true;
    return true;
}

//------------------------------------------------------------------------------
// The override mistake
//
// Assigning to a property an object inherits from a frozen prototype throws
// in strict code, so `MyError.prototype.name = "MyError"` or
// `obj.toString = f` would fail in a frozen realm. As in SES, these few
// properties become accessors first: the getter returns the original value,
// and the setter defines an own property on the object assigned to (it
// still refuses the built-in object itself).
//------------------------------------------------------------------------------

constexpr const char* kObjectPrototypeProps[] = {"constructor", "toString", "toLocaleString",
                                                 "valueOf", nullptr};
constexpr const char* kFunctionPrototypeProps[] = {"constructor", "toString", nullptr};
constexpr const char* kErrorPrototypeProps[] = {"constructor", "name", "message", "toString",
                                                nullptr};

struct Enablement
{
    const char* global;
    const char* const* props;  // on global.prototype
};

constexpr Enablement kEnablements[] = {
    {"Object", kObjectPrototypeProps},
    {"Function", kFunctionPrototypeProps},
    {"Error", kErrorPrototypeProps},
    {"EvalError", kErrorPrototypeProps},
    {"RangeError", kErrorPrototypeProps},
    {"ReferenceError", kErrorPrototypeProps},
    {"SyntaxError", kErrorPrototypeProps},
    {"TypeError", kErrorPrototypeProps},
    {"URIError", kErrorPrototypeProps},
    {"AggregateError", kErrorPrototypeProps},
};
// Deliberately not Array, Promise, RegExp and the collections: their
// prototypes back the engine's fast paths, and assigning `constructor` or
// `toString` to an object inheriting from them is rare.

bool EnablementGetter(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().set(js::GetFunctionNativeReserved(&args.callee(), 0));
    return true;
}

bool EnablementSetter(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    JS::RootedObject callee(cx, &args.callee());
    JS::RootedValue home(cx, js::GetFunctionNativeReserved(callee, 0));
    JS::RootedValue key(cx, js::GetFunctionNativeReserved(callee, 1));
    JS::RootedId id(cx);

    if (!JS_ValueToId(cx, key, &id))
    {
        return false;
    }

    if (!args.thisv().isObject() || (home.isObject() && &args.thisv().toObject() == &home.toObject()))
    {
        String text;
        text.Append(StringView("Cannot assign to read only property '")).Ignore();

        if (key.isString() && !AppendStringUtf8(cx, key.toString(), text))
        {
            JS_ClearPendingException(cx);
        }

        text.Append(StringView("' of a frozen built-in object")).Ignore();
        return ThrowTypeError(cx, text.CStr());
    }

    JS::RootedObject self(cx, &args.thisv().toObject());
    JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> own(cx);

    if (!JS_GetOwnPropertyDescriptorById(cx, self, id, &own))
    {
        return false;
    }

    if (own.isSome())
    {
        JS::ObjectOpResult result;

        if (!JS_ForwardSetPropertyTo(cx, self, id, args.get(0), args.thisv(), result))
        {
            return false;
        }

        if (!result.ok())
        {
            return result.reportError(cx, self, id);
        }
    }
    else if (!JS_DefinePropertyById(cx, self, id, args.get(0), JSPROP_ENUMERATE))
    {
        return false;
    }

    args.rval().setUndefined();
    return true;
}

/// Applies kEnablements in the current realm. Call before freezing.
bool ApplyEnablements(JSContext* cx, JS::HandleObject global)
{
    for (const Enablement& entry : kEnablements)
    {
        JS::RootedValue ctor(cx);

        if (!JS_GetProperty(cx, global, entry.global, &ctor))
        {
            return false;
        }

        if (!ctor.isObject())
        {
            continue;  // not in this build
        }

        JS::RootedObject ctorObj(cx, &ctor.toObject());
        JS::RootedValue protoValue(cx);

        if (!JS_GetProperty(cx, ctorObj, "prototype", &protoValue))
        {
            return false;
        }

        if (!protoValue.isObject())
        {
            continue;
        }

        JS::RootedObject target(cx, &protoValue.toObject());

        for (const char* const* prop = entry.props; *prop != nullptr; ++prop)
        {
            JS::RootedString name(cx, JS_AtomizeString(cx, *prop));
            JS::RootedId id(cx);
            JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx);

            if (!name || !JS_StringToId(cx, name, &id) ||
                !JS_GetOwnPropertyDescriptorById(cx, target, id, &desc))
            {
                return false;
            }

            if (desc.isNothing() || !desc->isDataDescriptor() || !desc->writable() ||
                !desc->configurable())
            {
                continue;
            }

            JSFunction* getter = js::NewFunctionWithReserved(cx, EnablementGetter, 0, 0, *prop);
            JSFunction* setter = getter ? js::NewFunctionWithReserved(cx, EnablementSetter, 1, 0, *prop)
                                        : nullptr;

            if (!setter)
            {
                return false;
            }

            JS::RootedObject getterObj(cx, JS_GetFunctionObject(getter));
            JS::RootedObject setterObj(cx, JS_GetFunctionObject(setter));
            js::SetFunctionNativeReserved(getterObj, 0, desc->value());
            js::SetFunctionNativeReserved(setterObj, 0, JS::ObjectValue(*target));
            js::SetFunctionNativeReserved(setterObj, 1, JS::StringValue(name));

            if (!JS_DefinePropertyById(cx, target, id, getterObj, setterObj,
                                       desc->enumerable() ? JSPROP_ENUMERATE : 0) ||
                !JS_FreezeObject(cx, getterObj) || !JS_FreezeObject(cx, setterObj))
            {
                return false;
            }
        }
    }

    return true;
}

/// Freezes every walked intrinsic of the current realm.
bool FreezeIntrinsics(JSContext* cx, RealmInfo& info, JS::HandleObject global)
{
    if (!ApplyEnablements(cx, global))
    {
        return false;
    }

    JS::RootedObject list(cx, info.intrinsics);
    uint32_t length = 0;

    if (!JS::GetArrayLength(cx, list, &length))
    {
        return false;
    }

    for (uint32_t i = 0; i < length; ++i)
    {
        JS::RootedValue item(cx);

        if (!JS_GetElement(cx, list, i, &item))
        {
            return false;
        }

        JS::RootedObject obj(cx, &item.toObject());

        if (!JS_FreezeObject(cx, obj))
        {
            return false;
        }
    }

    info.frozen = true;
    return true;
}

bool IsStandardName(StringView name)
{
    for (Size i = 0; i < g_state->standardNames.Size(); ++i)
    {
        if (g_state->standardNames[i].View() == name)
        {
            return true;
        }
    }

    return false;
}

/// Marks Forge's objects reachable from `value`: every function, and every
/// object that (directly or not) holds one, such as `fs`, `console` or
/// `Forge`. Plain data (Forge.args) and intrinsics are left alone. With
/// `freeze`, everything visited is frozen too.
bool MarkForgeValue(JSContext* cx, RealmInfo& info, JS::HandleValue value, bool freeze,
                    unsigned depth, bool* marked)
{
    *marked = false;

    if (!value.isObject() || depth > 8)
    {
        return true;
    }

    JS::RootedObject obj(cx, &value.toObject());
    int32_t kind = kUnknown;

    if (JS_IsGlobalObject(obj))
    {
        return true;
    }

    if (!KindOf(cx, info, obj, &kind))
    {
        return false;
    }

    if (kind == kForgeObject)
    {
        *marked = true;
        return true;
    }

    if (kind >= 0)
    {
        return true;  // an intrinsic
    }

    bool holdsForgeObject = JS::IsCallable(obj);

    if (!holdsForgeObject || !JS_ObjectIsFunction(obj))
    {
        JS::RootedIdVector keys(cx);

        if (!js::GetPropertyKeys(cx, obj, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &keys))
        {
            return false;
        }

        for (size_t i = 0; i < keys.length(); ++i)
        {
            JS::RootedId id(cx, keys[i]);
            JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx);

            if (!JS_GetOwnPropertyDescriptorById(cx, obj, id, &desc))
            {
                return false;
            }

            if (desc.isNothing())
            {
                continue;
            }

            JS::RootedValue item(cx);

            for (int part = 0; part < 3; ++part)
            {
                if (part == 0)
                {
                    item = desc->hasValue() ? desc->value() : JS::UndefinedValue();
                }
                else if (part == 1)
                {
                    item = desc->hasGetter() && desc->getter() ? JS::ObjectValue(*desc->getter())
                                                               : JS::UndefinedValue();
                }
                else
                {
                    item = desc->hasSetter() && desc->setter() ? JS::ObjectValue(*desc->setter())
                                                               : JS::UndefinedValue();
                }

                bool childMarked = false;

                if (!MarkForgeValue(cx, info, item, freeze, depth + 1, &childMarked))
                {
                    return false;
                }

                holdsForgeObject = holdsForgeObject || childMarked;
            }
        }
    }

    if (holdsForgeObject)
    {
        if (!SetKind(cx, info, obj, kForgeObject))
        {
            return false;
        }

        *marked = true;
    }

    return !freeze || JS_FreezeObject(cx, obj);
}

/// Marks everything Forge put on the global (every non-standard name).
bool MarkForgeGlobals(JSContext* cx, RealmInfo& info, JS::HandleObject global, bool freeze)
{
    JS::RootedIdVector keys(cx);

    if (!js::GetPropertyKeys(cx, global, JSITER_OWNONLY | JSITER_HIDDEN, &keys))
    {
        return false;
    }

    for (size_t i = 0; i < keys.length(); ++i)
    {
        if (!keys[i].isString())
        {
            continue;
        }

        String name;

        if (!AppendStringUtf8(cx, keys[i].toString(), name))
        {
            return false;
        }

        if (IsStandardName(name.View()))
        {
            continue;
        }

        JS::RootedId id(cx, keys[i]);
        JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx);

        if (!JS_GetOwnPropertyDescriptorById(cx, global, id, &desc))
        {
            return false;
        }

        if (desc.isNothing() || !desc->hasValue())
        {
            continue;
        }

        JS::RootedValue value(cx, desc->value());
        bool marked = false;

        if (!MarkForgeValue(cx, info, value, freeze, 0, &marked))
        {
            return false;
        }
    }

    return true;
}

/// Leaves `exception` pending in the current realm.
void RethrowInCurrentRealm(JSContext* cx, JS::HandleValue exception)
{
    JS::RootedValue value(cx, exception);

    if (JS_WrapValue(cx, &value))
    {
        JS_SetPendingException(cx, value);
    }
}

/// Makes sure compartment slot `index` exists (empty until filled).
bool EnsureSlot(Size index)
{
    while (g_state->realms.Size() <= index)
    {
        if (g_state->realms.PushBack(UniquePtr<RealmInfo>()).HasError())
        {
            return false;
        }
    }

    return true;
}

/// A new record in slot `index`, replacing any.
RealmInfo* NewRealmInfo(JSContext* cx, Size index, RealmKind kind,
                        const forge::kavach::PackagePolicy* policy)
{
    Result<UniquePtr<RealmInfo>> made = forge::core::memory::MakeUnique<RealmInfo>();

    if (made.HasError() || !EnsureSlot(index))
    {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    made.Value()->kind = kind;
    made.Value()->policy = policy;
    RealmInfo* info = made.Value().Get();
    g_state->realms[index] = std::move(made.Value());
    return info;
}

bool InstallRealmInfo(JSContext* cx, JS::HandleObject global, RealmKind kind, Size index)
{
    RealmInfo* info = NewRealmInfo(cx, index, kind, nullptr);

    if (info == nullptr)
    {
        return false;
    }

    info->global = global;
    JS::SetRealmPrivate(js::GetNonCCWObjectRealm(global), info);
    return true;
}

/// Creates the global of dependency compartment `index`: the shared pure
/// compartment, or (Phase 9.4) a permission-holding package's, whose
/// record already holds its policy.
JSObject* CreateDependencyGlobal(JSContext* cx, Size index)
{
    RealmInfo* app = g_state->realms.Size() > kAppCompartment
                         ? g_state->realms[kAppCompartment].Get()
                         : nullptr;
    const bool pure = index == kPureCompartment;
    bool (*defineGlobals)(JSContext*, JS::HandleObject) =
        pure ? g_state->hooks.defineDependencyGlobals : g_state->hooks.definePackageGlobals;

    // The program's intrinsics must be known before a package exists, or one
    // of them could reach the package as an ordinary wrapper.
    if (app == nullptr || !app->walked || g_state->hooks.newGlobal == nullptr ||
        defineGlobals == nullptr)
    {
        JS_ReportErrorASCII(cx, "Forge: dependencies can be loaded only by a program whose "
                                "forge.json lists them");
        return nullptr;
    }

    RealmInfo* info = pure ? NewRealmInfo(cx, index, RealmKind::Dependency,
                                          forge::kavach::NoPermissionPolicy())
                           : g_state->realms[index].Get();

    if (info == nullptr)
    {
        return nullptr;
    }

    const forge::kavach::PackagePolicy* policy = info->policy;
    JS::RootedObject appGlobal(cx, app->global);
    JS::RootedObject global(cx, g_state->hooks.newGlobal(cx, appGlobal));

    if (!global)
    {
        return nullptr;
    }

    info->global = global;
    JS::SetRealmPrivate(js::GetNonCCWObjectRealm(global), info);
    bool ok = false;
    bool thrown = false;
    JS::RootedValue exception(cx);

    {
        JSAutoRealm ar(cx, global);
        ok = JS::InitRealmStandardClasses(cx) && Walk(cx, *info, global) &&
             FreezeIntrinsics(cx, *info, global) && defineGlobals(cx, global) &&
             MarkForgeGlobals(cx, *info, global, true) && JS_FreezeObject(cx, global);

        if (!ok)
        {
            thrown = TakePendingException(cx, &exception);
        }
    }

    if (!ok)
    {
        // The realm is unusable; forget it so nothing runs in it. A package
        // keeps an empty record with its policy.
        JS::SetRealmPrivate(js::GetNonCCWObjectRealm(global), nullptr);

        if (pure)
        {
            g_state->realms[index] = UniquePtr<RealmInfo>();
        }
        else
        {
            NewRealmInfo(cx, index, RealmKind::Dependency, policy);
        }

        if (thrown)
        {
            RethrowInCurrentRealm(cx, exception);
        }

        return nullptr;
    }

    return global;
}

//------------------------------------------------------------------------------
// The wrapper policy
//------------------------------------------------------------------------------

/// The kind of `obj` in its own realm, looking through bound functions (a
/// bound Forge function is still a Forge function).
bool KindAcrossBinds(JSContext* cx, JSObject* start, RealmInfo** realm, int32_t* kind)
{
    *kind = kUnknown;
    *realm = nullptr;
    JS::RootedObject obj(cx, start);

    for (int hops = 0; obj && hops < 16; ++hops)
    {
        RealmInfo* info = InfoOfObject(obj);

        if (info == nullptr)
        {
            return true;
        }

        int32_t found = kUnknown;
        JS::RootedValue exception(cx);
        bool ok = false;

        {
            JSAutoRealm ar(cx, obj);
            ok = KindOf(cx, *info, obj, &found);

            if (!ok)
            {
                TakePendingException(cx, &exception);
            }
        }

        if (!ok)
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }

        if (hops == 0)
        {
            *realm = info;
            *kind = found;
        }

        if (found == kForgeObject)
        {
            *kind = kForgeObject;
            return true;
        }

        if (!JS_ObjectIsBoundFunction(obj))
        {
            return true;
        }

        obj = JS_GetBoundFunctionTarget(obj);

        if (obj && js::IsCrossCompartmentWrapper(obj))
        {
            obj = js::UncheckedUnwrap(obj);
        }
    }

    return true;
}

void RefuseCrossing(JSContext* cx, JS::HandleObject obj, int32_t kind, JS::MutableHandleObject retObj)
{
    String text;

    if (JS_IsGlobalObject(obj))
    {
        text.Append(StringView("A global object cannot be passed to another package")).Ignore();
    }
    else if (kind == kForgeObject)
    {
        JSFunction* fun = JS_GetObjectFunction(obj);
        JSString* name = fun ? JS_GetMaybePartialFunctionId(fun) : nullptr;

        if (name != nullptr)
        {
            text.Append(StringView("Forge's own function ")).Ignore();
            if (!AppendStringUtf8(cx, name, text))
            {
                JS_ClearPendingException(cx);
            }

            text.Append(StringView("() cannot be passed to another package")).Ignore();
        }
        else
        {
            text.Append(StringView("Forge's own objects and functions (fs, console, Forge and their "
                                   "functions) cannot be passed to another package")).Ignore();
        }
    }
    else
    {
        text.Append(StringView("A built-in object of the program (such as Object.prototype or "
                               "Function) cannot be passed to another package")).Ignore();
    }

    JS_ClearPendingException(cx);
    ThrowTypeError(cx, text.CStr());
    DefineStringOnPendingError(cx, "code", StringView("PermissionDenied"));
    retObj.set(nullptr);
}

/// JSPreWrapCallback: runs for every object about to cross into the current
/// compartment (cx's), already unwrapped.
void PreWrap(JSContext* cx, JS::HandleObject scope, JS::HandleObject origObj, JS::HandleObject obj,
             JS::HandleObject objectPassedToWrap, JS::MutableHandleObject retObj)
{
    (void)scope;
    (void)origObj;
    (void)objectPassedToWrap;
    retObj.set(obj);

    if (g_state == nullptr)
    {
        return;
    }

    RealmInfo* target = InfoOf(JS::GetCurrentRealmOrNull(cx));

    if (target == nullptr || InfoOfObject(obj) == nullptr)
    {
        return;  // a realm Forge does not manage
    }

    if (JS_IsGlobalObject(obj))
    {
        RefuseCrossing(cx, obj, kUnknown, retObj);
        return;
    }

    RealmInfo* source = nullptr;
    int32_t kind = kUnknown;

    if (!KindAcrossBinds(cx, obj, &source, &kind))
    {
        retObj.set(nullptr);
        return;
    }

    if (kind == kForgeObject)
    {
        RefuseCrossing(cx, obj, kind, retObj);
        return;
    }

    if (kind < 0 || source == nullptr)
    {
        return;  // an ordinary object: a transparent wrapper
    }

    // An intrinsic: hand over the receiving realm's own one.
    const Size index = static_cast<Size>(kind);

    if (target->walked && index < target->signatures.Size() && index < source->signatures.Size() &&
        target->signatures[index] == source->signatures[index])
    {
        JS::RootedObject list(cx, target->intrinsics);
        JS::RootedValue replacement(cx);

        if (!JS_GetElement(cx, list, static_cast<uint32_t>(index), &replacement))
        {
            retObj.set(nullptr);
            return;
        }

        if (replacement.isObject())
        {
            retObj.set(&replacement.toObject());
            return;
        }
    }

    // No counterpart. A frozen intrinsic may cross as a wrapper; a mutable
    // one of the program must never reach a package.
    if (!source->frozen && target->kind == RealmKind::Dependency)
    {
        RefuseCrossing(cx, obj, kind, retObj);
    }
}

JSObject* WrapObject(JSContext* cx, JS::HandleObject existing, JS::HandleObject obj)
{
    (void)existing;
    return js::Wrapper::New(cx, obj, &js::CrossCompartmentWrapper::singleton);
}

const JSWrapObjectCallbacks kWrapCallbacks = {WrapObject, PreWrap};

} // namespace

bool InitCompartments(JSContext* cx, const CompartmentHooks& hooks)
{
    if (g_state == nullptr)
    {
        Result<UniquePtr<State>> made = forge::core::memory::MakeUnique<State>();

        if (made.HasError())
        {
            return false;
        }

        g_state = made.Value().Release();
    }

    g_state->hooks = hooks;

    if (!g_state->tracing)
    {
        if (!JS_AddExtraGCRootsTracer(cx, TraceCompartments, nullptr))
        {
            return false;
        }

        g_state->tracing = true;
    }

    JS_SetWrapObjectCallbacks(cx, &kWrapCallbacks);
    return true;
}

void ShutdownCompartments(JSContext* cx)
{
    if (g_state == nullptr)
    {
        return;
    }

    for (Size i = 0; i < g_state->realms.Size(); ++i)
    {
        RealmInfo* info = g_state->realms[i].Get();

        if (info != nullptr && info->global.unbarrieredGet() != nullptr)
        {
            JS::SetRealmPrivate(js::GetNonCCWObjectRealm(info->global.unbarrieredGet()), nullptr);
        }
    }

    if (g_state->tracing)
    {
        JS_RemoveExtraGCRootsTracer(cx, TraceCompartments, nullptr);
    }

    delete g_state;
    g_state = nullptr;
}

bool SetStandardGlobalNames(const Vector<String>& names)
{
    if (g_state == nullptr)
    {
        return false;
    }

    g_state->standardNames.Clear();

    for (Size i = 0; i < names.Size(); ++i)
    {
        String copy;

        if (copy.Append(names[i].View()).HasError() ||
            g_state->standardNames.PushBack(std::move(copy)).HasError())
        {
            return false;
        }
    }

    return true;
}

bool PrepareAppRealm(JSContext* cx, JS::HandleObject global, bool isolate, bool freeze)
{
    if (g_state == nullptr)
    {
        JS_ReportErrorASCII(cx, "Forge: compartments are not initialized");
        return false;
    }

    if ((g_state->realms.Size() <= kAppCompartment ||
         g_state->realms[kAppCompartment].Get() == nullptr) &&
        !InstallRealmInfo(cx, global, RealmKind::App, kAppCompartment))
    {
        return false;
    }

    RealmInfo& info = *g_state->realms[kAppCompartment];

    if ((isolate || freeze) && !info.walked)
    {
        if (!Walk(cx, info, global) || !MarkForgeGlobals(cx, info, global, false))
        {
            return false;
        }
    }

    return !freeze || info.frozen || FreezeIntrinsics(cx, info, global);
}

JSObject* CompartmentGlobal(JSContext* cx, Size index)
{
    if (g_state == nullptr || index >= g_state->realms.Size() + (index == kPureCompartment ? 1 : 0))
    {
        JS_ReportErrorASCII(cx, "Forge: unknown compartment");
        return nullptr;
    }

    RealmInfo* info = index < g_state->realms.Size() ? g_state->realms[index].Get() : nullptr;

    if (info != nullptr && info->global.unbarrieredGet() != nullptr)
    {
        return info->global;
    }

    if (index == kPureCompartment || (info != nullptr && info->kind == RealmKind::Dependency))
    {
        return CreateDependencyGlobal(cx, index);
    }

    JS_ReportErrorASCII(cx, "Forge: the program's compartment is not set up");
    return nullptr;
}

bool AddPackageCompartment(const forge::kavach::PackagePolicy* policy, Size& index)
{
    index = 0;

    if (g_state == nullptr || !EnsureSlot(kPureCompartment))
    {
        return false;
    }

    Result<UniquePtr<RealmInfo>> made = forge::core::memory::MakeUnique<RealmInfo>();

    if (made.HasError())
    {
        return false;
    }

    made.Value()->kind = RealmKind::Dependency;
    made.Value()->policy = policy;
    index = g_state->realms.Size();

    if (g_state->realms.PushBack(std::move(made.Value())).HasError())
    {
        index = 0;
        return false;
    }

    return true;
}

const forge::kavach::PackagePolicy* CurrentPolicy(JSContext* cx)
{
    const RealmInfo* info = InfoOf(JS::GetCurrentRealmOrNull(cx));
    return info != nullptr ? info->policy : nullptr;
}

bool IsDependencyRealm(JSContext* cx)
{
    const RealmInfo* info = InfoOf(JS::GetCurrentRealmOrNull(cx));
    return info != nullptr && info->kind == RealmKind::Dependency;
}

bool MarkHostObject(JSContext* cx, JS::HandleObject obj)
{
    RealmInfo* info = InfoOfObject(obj);

    if (info == nullptr || !info->walked)
    {
        return true;  // nothing can cross out of an isolated-free realm
    }

    return SetKind(cx, *info, obj, kForgeObject);
}

JSObject* RealmEntryFor(JSObject* callable)
{
    if (callable != nullptr && js::IsCrossCompartmentWrapper(callable))
    {
        return js::GetFirstGlobalInCompartment(JS::GetCompartment(callable));
    }

    return callable;
}

} // namespace forge::runtime
