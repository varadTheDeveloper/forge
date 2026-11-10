#include "runtime/Inspect.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "js/Array.h"
#include "js/ArrayBuffer.h"
#include "js/CallAndConstruct.h"
#include "js/CharacterEncoding.h"
#include "js/Class.h"
#include "js/Conversions.h"
#include "js/Date.h"
#include "js/Exception.h"
#include "js/GCVector.h"
#include "js/MapAndSet.h"
#include "js/Object.h"
#include "js/Promise.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"
#include "js/Proxy.h"
#include "js/RegExp.h"
#include "js/RegExpFlags.h"
#include "js/Stack.h"
#include "js/String.h"
#include "js/Symbol.h"
#include "js/experimental/TypedData.h"
#include "jsapi.h"
#include "jsfriendapi.h"

#include "forge-core/memory/Vector.h"

namespace forge::runtime
{

namespace
{

using forge::core::Size;
using forge::core::String;
using forge::core::StringView;

/// Lines longer than this are broken up, one entry per line.
constexpr Size kLineWidth = 72;

/// Array, Map, Set and typed-array entries shown before "... N more items".
constexpr uint32_t kMaxEntries = 100;

/// Characters of a nested string shown before "... N more characters".
constexpr Size kMaxNestedString = 10000;

/// Appends to a String and remembers whether any append ran out of memory,
/// so the many small appends below need no individual checks.
class Text
{
public:
    explicit Text(String& out) : out_(out) {}

    void Add(StringView text)
    {
        if (ok_ && out_.Append(text).HasError())
        {
            ok_ = false;
        }
    }

    void Add(char c)
    {
        if (ok_ && out_.Append(c).HasError())
        {
            ok_ = false;
        }
    }

    void AddUnsigned(uint64_t value)
    {
        char buffer[24];
        const int n = snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
        Add(StringView(buffer, static_cast<Size>(n)));
    }

    void AddSpaces(unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
        {
            Add(' ');
        }
    }

    void Fail() { ok_ = false; }
    bool Ok() const { return ok_; }
    String& Str() { return out_; }

private:
    String& out_;
    bool ok_ = true;
};

bool AppendJSString(
    JSContext* cx,
    JSString* str,
    Text& out)
{
    JS::RootedString rooted(cx, str);
    JSLinearString* linear = JS_EnsureLinearString(cx, rooted);

    if (!linear)
    {
        return false;
    }

    const size_t length = JS::GetDeflatedUTF8StringLength(linear);

    if (length == 0)
    {
        return true;
    }

    // DeflateStringToUTF8Buffer needs a writable buffer of the exact size;
    // String cannot be resized, so convert into a Vector first. (Unlike
    // JS_EncodeStringToUTF8 this keeps embedded NUL characters.)
    forge::core::Vector<char> buffer;

    if (buffer.Reserve(length).HasError())
    {
        out.Fail();
        return true;
    }

    for (size_t i = 0; i < length; ++i)
    {
        if (buffer.PushBack('\0').HasError())
        {
            out.Fail();
            return true;
        }
    }

    const size_t written =
        JS::DeflateStringToUTF8Buffer(linear, mozilla::Span<char>(buffer.Data(), length));
    out.Add(StringView(buffer.Data(), written));
    return true;
}

/// Appends `utf8` as a quoted JavaScript string literal.
void AppendQuoted(
    StringView utf8,
    Text& out)
{
    bool hasSingle = false;
    bool hasDouble = false;
    bool hasBacktick = false;

    for (Size i = 0; i < utf8.Size(); ++i)
    {
        hasSingle = hasSingle || utf8.Data()[i] == '\'';
        hasDouble = hasDouble || utf8.Data()[i] == '"';
        hasBacktick = hasBacktick || utf8.Data()[i] == '`';
    }

    char quote = '\'';

    if (hasSingle && !hasDouble)
    {
        quote = '"';
    }
    else if (hasSingle && hasDouble && !hasBacktick)
    {
        quote = '`';
    }

    out.Add(quote);

    Size shown = utf8.Size() > kMaxNestedString ? kMaxNestedString : utf8.Size();

    // Do not cut a multi-byte character in half.
    while (shown < utf8.Size() && shown > 0 &&
           (static_cast<unsigned char>(utf8.Data()[shown]) & 0xC0) == 0x80)
    {
        --shown;
    }

    for (Size i = 0; i < shown; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(utf8.Data()[i]);

        switch (c)
        {
        case '\n': out.Add("\\n"); continue;
        case '\t': out.Add("\\t"); continue;
        case '\r': out.Add("\\r"); continue;
        case '\b': out.Add("\\b"); continue;
        case '\f': out.Add("\\f"); continue;
        case '\v': out.Add("\\v"); continue;
        case '\\': out.Add("\\\\"); continue;
        default: break;
        }

        if (c == static_cast<unsigned char>(quote))
        {
            out.Add('\\');
            out.Add(static_cast<char>(c));
        }
        else if (c < 0x20 || c == 0x7F)
        {
            char escape[8];
            snprintf(escape, sizeof(escape), "\\x%02X", c);
            out.Add(escape);
        }
        else
        {
            out.Add(static_cast<char>(c));
        }
    }

    out.Add(quote);

    if (shown < utf8.Size())
    {
        out.Add("... ");
        out.AddUnsigned(utf8.Size() - shown);
        out.Add(" more bytes");
    }
}

bool IsIdentifier(
    StringView name)
{
    if (name.Empty())
    {
        return false;
    }

    for (Size i = 0; i < name.Size(); ++i)
    {
        const char c = name.Data()[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
        const bool digit = c >= '0' && c <= '9';

        if (!letter && !(digit && i > 0))
        {
            return false;
        }
    }

    return true;
}

/// Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
void CivilFromDays(
    int64_t days,
    int64_t* year,
    unsigned* month,
    unsigned* day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *day = doy - (153 * mp + 2) / 5 + 1;
    *month = mp < 10 ? mp + 3 : mp - 9;
    *year = static_cast<int64_t>(yoe) + era * 400 + (*month <= 2 ? 1 : 0);
}

/// Date.prototype.toISOString's format, computed here so no script runs.
void AppendIsoDate(
    double ms,
    Text& out)
{
    if (!isfinite(ms))
    {
        out.Add("Invalid Date");
        return;
    }

    const int64_t total = static_cast<int64_t>(ms);
    int64_t days = total / 86400000;
    int64_t rest = total % 86400000;

    if (rest < 0)
    {
        rest += 86400000;
        --days;
    }

    int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    CivilFromDays(days, &year, &month, &day);

    char buffer[48];
    const unsigned msOfDay = static_cast<unsigned>(rest);
    const unsigned hours = msOfDay / 3600000;
    const unsigned minutes = (msOfDay / 60000) % 60;
    const unsigned seconds = (msOfDay / 1000) % 60;
    const unsigned millis = msOfDay % 1000;

    if (year >= 0 && year <= 9999)
    {
        snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02u.%03uZ",
                 static_cast<long long>(year), month, day, hours, minutes, seconds, millis);
    }
    else
    {
        snprintf(buffer, sizeof(buffer), "%c%06lld-%02u-%02uT%02u:%02u:%02u.%03uZ",
                 year < 0 ? '-' : '+', static_cast<long long>(year < 0 ? -year : year), month,
                 day, hours, minutes, seconds, millis);
    }

    out.Add(buffer);
}

/// Looks `name` up on `start` and its prototype chain, but only as a plain
/// data property: an accessor, a Proxy anywhere on the chain, or a missing
/// property all give *found = false. Never runs script.
bool GetDataProperty(
    JSContext* cx,
    JS::HandleObject start,
    const char* name,
    JS::MutableHandleValue value,
    bool* found)
{
    *found = false;
    value.setUndefined();

    JS::RootedString atom(cx, JS_AtomizeString(cx, name));

    if (!atom)
    {
        return false;
    }

    JS::RootedId id(cx);

    if (!JS_StringToId(cx, atom, &id))
    {
        return false;
    }

    JS::RootedObject obj(cx, start);

    for (int hops = 0; obj && hops < 1000; ++hops)
    {
        if (js::IsProxy(obj))
        {
            return true;
        }

        JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx);

        if (!JS_GetOwnPropertyDescriptorById(cx, obj, id, &desc))
        {
            return false;
        }

        if (desc.isSome())
        {
            if (desc->isDataDescriptor())
            {
                value.set(desc->value());
                *found = true;
            }

            return true;
        }

        if (!JS_GetPrototype(cx, obj, &obj))
        {
            return false;
        }
    }

    return true;
}

/// The function's name, or empty for an anonymous function.
bool AppendFunctionName(
    JSContext* cx,
    JSObject* fnObj,
    Text& out)
{
    if (!JS_ObjectIsFunction(fnObj))
    {
        return true;
    }

    JSString* name = JS_GetMaybePartialFunctionId(JS_GetObjectFunction(fnObj));

    if (!name)
    {
        return true;
    }

    return AppendJSString(cx, name, out);
}

/// Native callback for JS::MapForEach / JS::SetForEach: appends (key,
/// value) to the array passed as `this`. Uses define, not set, so no setter
/// on Array.prototype can run.
bool CollectEntry(
    JSContext* cx,
    unsigned argc,
    JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    if (!args.thisv().isObject())
    {
        return true;
    }

    JS::RootedObject list(cx, &args.thisv().toObject());
    uint32_t length = 0;

    if (!JS::GetArrayLength(cx, list, &length))
    {
        return false;
    }

    if (length >= kMaxEntries * 2)
    {
        return true;
    }

    // forEach passes (value, key, collection).
    return JS_DefineElement(cx, list, length, args.get(1), JSPROP_ENUMERATE) &&
           JS_DefineElement(cx, list, length + 1, args.get(0), JSPROP_ENUMERATE);
}

class Inspector
{
public:
    Inspector(JSContext* cx, const InspectOptions& options)
        : cx_(cx), options_(options), seen_(cx)
    {
    }

    /// Renders `value` found at nesting level `depth` (0 = top level).
    /// `indent` is the indentation level of the line it starts on.
    bool Render(JS::HandleValue value, unsigned depth, unsigned indent, Text& out)
    {
        if (value.isUndefined())
        {
            out.Add("undefined");
            return true;
        }

        if (value.isNull())
        {
            out.Add("null");
            return true;
        }

        if (value.isBoolean())
        {
            out.Add(value.toBoolean() ? "true" : "false");
            return true;
        }

        if (value.isNumber())
        {
            return RenderNumber(value, out);
        }

        if (value.isString())
        {
            if (depth == 0 && options_.rawTopLevelStrings)
            {
                return AppendJSString(cx_, value.toString(), out);
            }

            String utf8;
            Text text(utf8);

            if (!AppendJSString(cx_, value.toString(), text))
            {
                return false;
            }

            if (!text.Ok())
            {
                out.Fail();
                return true;
            }

            AppendQuoted(utf8.View(), out);
            return true;
        }

        if (value.isSymbol())
        {
            return RenderSymbol(value, out);
        }

        if (value.isBigInt())
        {
            // ToString of a BigInt primitive runs no script.
            JS::RootedString str(cx_, JS::ToString(cx_, value));

            if (!str || !AppendJSString(cx_, str, out))
            {
                return false;
            }

            out.Add('n');
            return true;
        }

        if (value.isObject())
        {
            JS::RootedObject obj(cx_, &value.toObject());
            return RenderObject(obj, depth, indent, out);
        }

        out.Add("[unknown]");
        return true;
    }

private:
    bool RenderNumber(JS::HandleValue value, Text& out)
    {
        if (value.isDouble() && value.toDouble() == 0 && signbit(value.toDouble()))
        {
            out.Add("-0");
            return true;
        }

        // ToString of a number primitive runs no script.
        JS::RootedString str(cx_, JS::ToString(cx_, value));
        return str && AppendJSString(cx_, str, out);
    }

    bool RenderSymbol(JS::HandleValue value, Text& out)
    {
        JS::RootedSymbol symbol(cx_, value.toSymbol());
        out.Add("Symbol(");
        JSString* description = JS::GetSymbolDescription(cx_, symbol);

        if (description && !AppendJSString(cx_, description, out))
        {
            return false;
        }

        out.Add(')');
        return true;
    }

    bool RenderKey(JS::HandleId id, Text& out)
    {
        JS::RootedValue key(cx_);

        if (!JS_IdToValue(cx_, id, &key))
        {
            return false;
        }

        if (key.isSymbol())
        {
            out.Add('[');

            if (!RenderSymbol(key, out))
            {
                return false;
            }

            out.Add(']');
            return true;
        }

        if (key.isInt32())
        {
            out.AddUnsigned(static_cast<uint64_t>(key.toInt32()));
            return true;
        }

        if (!key.isString())
        {
            out.Add("[key]");
            return true;
        }

        String utf8;
        Text text(utf8);

        if (!AppendJSString(cx_, key.toString(), text))
        {
            return false;
        }

        if (!text.Ok())
        {
            out.Fail();
            return true;
        }

        if (IsIdentifier(utf8.View()))
        {
            out.Add(utf8.View());
        }
        else
        {
            AppendQuoted(utf8.View(), out);
        }

        return true;
    }

    /// Renders "key: value" for an own property, or nothing if the
    /// property disappeared. Accessors show as [Getter], [Setter] or
    /// [Getter/Setter] and are never called.
    bool RenderProperty(JS::HandleObject obj, JS::HandleId id, unsigned depth, unsigned indent,
                        Text& out)
    {
        JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx_);

        if (!JS_GetOwnPropertyDescriptorById(cx_, obj, id, &desc))
        {
            return false;
        }

        if (desc.isNothing())
        {
            return true;
        }

        if (!RenderKey(id, out))
        {
            return false;
        }

        out.Add(": ");

        if (desc->isAccessorDescriptor())
        {
            const bool getter = desc->hasGetter() && desc->getter();
            const bool setter = desc->hasSetter() && desc->setter();
            out.Add(getter && setter ? "[Getter/Setter]"
                    : getter         ? "[Getter]"
                    : setter         ? "[Setter]"
                                     : "undefined");
            return true;
        }

        JS::RootedValue value(cx_, desc->value());
        return Render(value, depth + 1, indent + 1, out);
    }

    /// Joins entries as "{ a, b }" on one line if short enough, otherwise one
    /// entry per line. `prefix` comes before the opening bracket.
    void Join(StringView prefix, const char* open, const char* close,
              forge::core::Vector<String>& entries, unsigned indent, Text& out)
    {
        out.Add(prefix);

        if (entries.Empty())
        {
            out.Add(open);
            out.Add(close);
            return;
        }

        Size total = prefix.Size() + 4 + static_cast<Size>(indent) * 2;
        bool multiline = false;

        for (Size i = 0; i < entries.Size(); ++i)
        {
            total += entries[i].Size() + 2;
            const StringView view = entries[i].View();

            for (Size j = 0; j < view.Size() && !multiline; ++j)
            {
                multiline = view.Data()[j] == '\n';
            }
        }

        if (!multiline && total <= kLineWidth)
        {
            out.Add(open);
            out.Add(' ');

            for (Size i = 0; i < entries.Size(); ++i)
            {
                if (i > 0)
                {
                    out.Add(", ");
                }

                out.Add(entries[i].View());
            }

            out.Add(' ');
            out.Add(close);
            return;
        }

        out.Add(open);
        out.Add('\n');

        for (Size i = 0; i < entries.Size(); ++i)
        {
            out.AddSpaces((indent + 1) * 2);
            out.Add(entries[i].View());
            out.Add(i + 1 < entries.Size() ? ",\n" : "\n");
        }

        out.AddSpaces(indent * 2);
        out.Add(close);
    }

    /// Adds a new, empty entry and returns a Text that writes into it.
    String* NewEntry(forge::core::Vector<String>& entries, Text& out)
    {
        if (entries.PushBack(String()).HasError())
        {
            out.Fail();
            return nullptr;
        }

        return &entries[entries.Size() - 1];
    }

    /// Own enumerable properties (strings and symbols) as entries, skipping
    /// array indices when `skipIndices` and the names in `skip`.
    bool CollectOwnProperties(JS::HandleObject obj, bool skipIndices, const char* const* skip,
                              unsigned depth, unsigned indent,
                              forge::core::Vector<String>& entries, Text& out)
    {
        JS::RootedIdVector ids(cx_);

        if (!js::GetPropertyKeys(cx_, obj, JSITER_OWNONLY | JSITER_SYMBOLS, &ids))
        {
            return false;
        }

        JS::RootedId id(cx_);

        for (size_t i = 0; i < ids.length(); ++i)
        {
            id = ids[i];

            if (skipIndices && id.isInt())
            {
                continue;
            }

            if (skip && id.isString())
            {
                bool skipped = false;

                for (const char* const* name = skip; *name && !skipped; ++name)
                {
                    skipped = JS_LinearStringEqualsAscii(id.toLinearString(), *name);
                }

                if (skipped)
                {
                    continue;
                }
            }

            String* entry = NewEntry(entries, out);

            if (!entry)
            {
                return true;
            }

            Text text(*entry);

            if (!RenderProperty(obj, id, depth, indent, text))
            {
                return false;
            }

            if (!text.Ok())
            {
                out.Fail();
            }
        }

        return true;
    }

    /// "Foo" for an instance of class Foo, empty for plain objects, and
    /// "[Object: null prototype]" when there is no prototype.
    bool AppendConstructorPrefix(JS::HandleObject obj, Text& out)
    {
        JS::RootedObject proto(cx_);

        if (!JS_GetPrototype(cx_, obj, &proto))
        {
            return false;
        }

        if (!proto)
        {
            out.Add("[Object: null prototype] ");
            return true;
        }

        JS::RootedValue ctor(cx_);
        bool found = false;

        if (!GetDataProperty(cx_, proto, "constructor", &ctor, &found))
        {
            return false;
        }

        if (!found || !ctor.isObject())
        {
            return true;
        }

        String name;
        Text text(name);

        if (!AppendFunctionName(cx_, &ctor.toObject(), text))
        {
            return false;
        }

        if (!name.Empty() && !(name.View() == StringView("Object")))
        {
            out.Add(name.View());
            out.Add(' ');
        }

        return true;
    }

    bool RenderArrayLike(JS::HandleObject obj, uint32_t length, StringView prefix,
                         bool isTypedArray, unsigned depth, unsigned indent, Text& out)
    {
        forge::core::Vector<String> entries;
        const uint32_t shown = length > kMaxEntries ? kMaxEntries : length;
        uint32_t holes = 0;

        auto flushHoles = [&]() {
            if (holes == 0)
            {
                return;
            }

            String* entry = NewEntry(entries, out);

            if (entry)
            {
                Text text(*entry);
                text.Add('<');
                text.AddUnsigned(holes);
                text.Add(holes == 1 ? " empty item>" : " empty items>");

                if (!text.Ok())
                {
                    out.Fail();
                }
            }

            holes = 0;
        };

        JS::RootedValue element(cx_);
        JS::RootedId id(cx_);

        for (uint32_t i = 0; i < shown; ++i)
        {
            if (isTypedArray)
            {
                // Integer-indexed reads of a typed array never consult the
                // prototype chain, so this runs no script.
                if (!JS_GetElement(cx_, obj, i, &element))
                {
                    return false;
                }

                String* entry = NewEntry(entries, out);

                if (!entry)
                {
                    return true;
                }

                Text text(*entry);

                if (!Render(element, depth + 1, indent + 1, text))
                {
                    return false;
                }

                if (!text.Ok())
                {
                    out.Fail();
                }

                continue;
            }

            if (!JS_IndexToId(cx_, i, &id))
            {
                return false;
            }

            JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx_);

            if (!JS_GetOwnPropertyDescriptorById(cx_, obj, id, &desc))
            {
                return false;
            }

            if (desc.isNothing())
            {
                ++holes;
                continue;
            }

            flushHoles();
            String* entry = NewEntry(entries, out);

            if (!entry)
            {
                return true;
            }

            Text text(*entry);

            if (desc->isAccessorDescriptor())
            {
                text.Add("[Getter/Setter]");
            }
            else
            {
                element = desc->value();

                if (!Render(element, depth + 1, indent + 1, text))
                {
                    return false;
                }
            }

            if (!text.Ok())
            {
                out.Fail();
            }
        }

        flushHoles();

        if (length > shown)
        {
            String* entry = NewEntry(entries, out);

            if (entry)
            {
                Text text(*entry);
                text.Add("... ");
                text.AddUnsigned(length - shown);
                text.Add(length - shown == 1 ? " more item" : " more items");
            }
        }

        if (!isTypedArray)
        {
            static const char* const kSkip[] = { "length", nullptr };

            if (!CollectOwnProperties(obj, true, kSkip, depth, indent, entries, out))
            {
                return false;
            }
        }

        Join(prefix, "[", "]", entries, indent, out);
        return true;
    }

    bool RenderMapOrSet(JS::HandleObject obj, bool isMap, unsigned depth, unsigned indent,
                        Text& out)
    {
        const uint32_t size = isMap ? JS::MapSize(cx_, obj) : JS::SetSize(cx_, obj);

        JS::RootedObject list(cx_, JS::NewArrayObject(cx_, 0));
        JSFunction* collector = JS_NewFunction(cx_, CollectEntry, 3, 0, "collect");

        if (!list || !collector)
        {
            return false;
        }

        JS::RootedValue callback(cx_, JS::ObjectValue(*JS_GetFunctionObject(collector)));
        JS::RootedValue thisValue(cx_, JS::ObjectValue(*list));

        // The engine's own forEach walks the entries with internal
        // operations only; user code cannot intercept it. The callback is
        // the native CollectEntry above.
        const bool ok = isMap ? JS::MapForEach(cx_, obj, callback, thisValue)
                              : JS::SetForEach(cx_, obj, callback, thisValue);

        if (!ok)
        {
            return false;
        }

        uint32_t collected = 0;

        if (!JS::GetArrayLength(cx_, list, &collected))
        {
            return false;
        }

        forge::core::Vector<String> entries;
        JS::RootedValue key(cx_);
        JS::RootedValue value(cx_);

        for (uint32_t i = 0; i + 1 < collected; i += 2)
        {
            if (!JS_GetElement(cx_, list, i, &key) || !JS_GetElement(cx_, list, i + 1, &value))
            {
                return false;
            }

            String* entry = NewEntry(entries, out);

            if (!entry)
            {
                return true;
            }

            Text text(*entry);

            if (!Render(isMap ? key : value, depth + 1, indent + 1, text))
            {
                return false;
            }

            if (isMap)
            {
                text.Add(" => ");

                if (!Render(value, depth + 1, indent + 1, text))
                {
                    return false;
                }
            }

            if (!text.Ok())
            {
                out.Fail();
            }
        }

        if (size > collected / 2)
        {
            String* entry = NewEntry(entries, out);

            if (entry)
            {
                Text text(*entry);
                text.Add("... ");
                text.AddUnsigned(size - collected / 2);
                text.Add(" more items");
            }
        }

        String prefix;
        Text prefixText(prefix);
        prefixText.Add(isMap ? "Map(" : "Set(");
        prefixText.AddUnsigned(size);
        prefixText.Add(") ");

        if (!prefixText.Ok())
        {
            out.Fail();
            return true;
        }

        Join(prefix.View(), "{", "}", entries, indent, out);
        return true;
    }

    /// "Name: message", read from data properties only.
    bool AppendErrorHeader(JS::HandleObject obj, Text& out)
    {
        JS::RootedValue name(cx_);
        JS::RootedValue message(cx_);
        bool hasName = false;
        bool hasMessage = false;

        if (!GetDataProperty(cx_, obj, "name", &name, &hasName) ||
            !GetDataProperty(cx_, obj, "message", &message, &hasMessage))
        {
            return false;
        }

        if (hasName && name.isString())
        {
            if (!AppendJSString(cx_, name.toString(), out))
            {
                return false;
            }
        }
        else
        {
            out.Add("Error");
        }

        if (hasMessage && message.isString() && JS_GetStringLength(message.toString()) > 0)
        {
            out.Add(": ");

            if (!AppendJSString(cx_, message.toString(), out))
            {
                return false;
            }
        }

        return true;
    }

    bool RenderError(JS::HandleObject obj, unsigned depth, unsigned indent, Text& out)
    {
        if (depth > 0)
        {
            out.Add('[');

            if (!AppendErrorHeader(obj, out))
            {
                return false;
            }

            out.Add(']');
            return true;
        }

        if (!AppendErrorHeader(obj, out))
        {
            return false;
        }

        if (JSObject* stackObj = JS::ExceptionStackOrNull(obj))
        {
            JS::RootedObject stack(cx_, stackObj);
            JS::RootedString text(cx_);

            if (!JS::BuildStackString(cx_, nullptr, stack, &text, 4))
            {
                return false;
            }

            if (text && JS_GetStringLength(text) > 0)
            {
                out.Add('\n');
                String frames;
                Text framesText(frames);

                if (!AppendJSString(cx_, text, framesText))
                {
                    return false;
                }

                // BuildStackString ends every frame with a newline.
                StringView view = frames.View();

                while (!view.Empty() && view.Data()[view.Size() - 1] == '\n')
                {
                    view = StringView(view.Data(), view.Size() - 1);
                }

                out.Add(view);
            }
        }

        // Extra own properties, such as the `code` Forge puts on its errors.
        static const char* const kSkip[] = { "stack", "message", "fileName", "lineNumber",
                                             "columnNumber", nullptr };
        forge::core::Vector<String> entries;

        if (!CollectOwnProperties(obj, false, kSkip, depth, indent, entries, out))
        {
            return false;
        }

        if (!entries.Empty())
        {
            out.Add(' ');
            Join(StringView(), "{", "}", entries, indent, out);
        }

        return true;
    }

    bool RenderFunction(JS::HandleObject obj, unsigned depth, unsigned indent, Text& out)
    {
        String header;
        Text text(header);
        text.Add("[Function");

        String name;
        Text nameText(name);

        if (!AppendFunctionName(cx_, obj, nameText))
        {
            return false;
        }

        if (name.Empty())
        {
            text.Add(JS_ObjectIsFunction(obj) ? " (anonymous)]" : "]");
        }
        else
        {
            text.Add(": ");
            text.Add(name.View());
            text.Add(']');
        }

        if (!text.Ok() || !nameText.Ok())
        {
            out.Fail();
            return true;
        }

        forge::core::Vector<String> entries;

        if (depth <= options_.depth &&
            !CollectOwnProperties(obj, false, nullptr, depth, indent, entries, out))
        {
            return false;
        }

        out.Add(header.View());

        if (!entries.Empty())
        {
            out.Add(' ');
            Join(StringView(), "{", "}", entries, indent, out);
        }

        return true;
    }

    bool RenderPromise(JS::HandleObject obj, unsigned depth, unsigned indent, Text& out)
    {
        forge::core::Vector<String> entries;
        String* entry = NewEntry(entries, out);

        if (!entry)
        {
            return true;
        }

        Text text(*entry);
        const JS::PromiseState state = JS::GetPromiseState(obj);

        if (state == JS::PromiseState::Pending)
        {
            text.Add("<pending>");
        }
        else
        {
            if (state == JS::PromiseState::Rejected)
            {
                text.Add("<rejected> ");
            }

            JS::RootedValue result(cx_, JS::GetPromiseResult(obj));

            if (!Render(result, depth + 1, indent + 1, text))
            {
                return false;
            }
        }

        if (!text.Ok())
        {
            out.Fail();
        }

        Join("Promise ", "{", "}", entries, indent, out);
        return true;
    }

    bool RenderRegExp(JS::HandleObject obj, Text& out)
    {
        JS::RootedString source(cx_, JS::GetRegExpSource(cx_, obj));

        if (!source)
        {
            return false;
        }

        out.Add('/');

        if (!AppendJSString(cx_, source, out))
        {
            return false;
        }

        out.Add('/');
        const JS::RegExpFlags flags = JS::GetRegExpFlags(cx_, obj);

        if (flags.hasIndices()) out.Add('d');
        if (flags.global()) out.Add('g');
        if (flags.ignoreCase()) out.Add('i');
        if (flags.multiline()) out.Add('m');
        if (flags.dotAll()) out.Add('s');
        if (flags.unicode()) out.Add('u');
        if (flags.unicodeSets()) out.Add('v');
        if (flags.sticky()) out.Add('y');
        return true;
    }

    bool RenderObject(JS::HandleObject obj, unsigned depth, unsigned indent, Text& out)
    {
        // A Proxy's handler is script: asking it anything would run code.
        if (js::IsProxy(obj))
        {
            out.Add(JS::IsCallable(obj) ? "[Function (proxy)]" : "[Proxy]");
            return true;
        }

        for (size_t i = 0; i < seen_.length(); ++i)
        {
            if (seen_[i] == obj)
            {
                out.Add("[Circular]");
                return true;
            }
        }

        js::ESClass cls = js::ESClass::Other;

        if (!JS::GetBuiltinClass(cx_, obj, &cls))
        {
            return false;
        }

        // Things that print the same at any depth.
        switch (cls)
        {
        case js::ESClass::Date:
        {
            double ms = 0;

            if (!js::DateGetMsecSinceEpoch(cx_, obj, &ms))
            {
                return false;
            }

            AppendIsoDate(ms, out);
            return true;
        }
        case js::ESClass::RegExp:
            return RenderRegExp(obj, out);
        case js::ESClass::Error:
            if (depth > options_.depth)
            {
                depth = options_.depth + 1;  // header only
            }
            return RenderError(obj, depth, indent, out);
        case js::ESClass::Number:
            out.Add("[Number]");
            return true;
        case js::ESClass::String:
            out.Add("[String]");
            return true;
        case js::ESClass::Boolean:
            out.Add("[Boolean]");
            return true;
        case js::ESClass::BigInt:
            out.Add("[BigInt]");
            return true;
        case js::ESClass::MapIterator:
            out.Add("[Map Iterator]");
            return true;
        case js::ESClass::SetIterator:
            out.Add("[Set Iterator]");
            return true;
        case js::ESClass::ArrayBuffer:
        case js::ESClass::SharedArrayBuffer:
        {
            out.Add(cls == js::ESClass::ArrayBuffer ? "ArrayBuffer { byteLength: "
                                                    : "SharedArrayBuffer { byteLength: ");
            out.AddUnsigned(JS::IsArrayBufferObject(obj) ? JS::GetArrayBufferByteLength(obj) : 0);
            out.Add(" }");
            return true;
        }
        default:
            break;
        }

        if (depth > options_.depth)
        {
            if (cls == js::ESClass::Array)
            {
                out.Add("[Array]");
            }
            else if (JS::IsCallable(obj))
            {
                return RenderFunction(obj, depth, indent, out);
            }
            else
            {
                String prefix;
                Text text(prefix);

                if (!AppendConstructorPrefix(obj, text))
                {
                    return false;
                }

                // "Foo " -> "[Foo]"; plain objects -> "[Object]".
                StringView view = prefix.View();

                if (!view.Empty() && view.Data()[view.Size() - 1] == ' ')
                {
                    view = StringView(view.Data(), view.Size() - 1);
                }

                out.Add('[');
                out.Add(view.Empty() ? StringView("Object") : view);
                out.Add(']');
            }

            return true;
        }

        if (!seen_.append(obj))
        {
            return false;
        }

        bool ok = RenderObjectBody(obj, cls, depth, indent, out);
        seen_.popBack();
        return ok;
    }

    bool RenderObjectBody(JS::HandleObject obj, js::ESClass cls, unsigned depth,
                          unsigned indent, Text& out)
    {
        switch (cls)
        {
        case js::ESClass::Array:
        case js::ESClass::Arguments:
        {
            uint32_t length = 0;

            if (cls == js::ESClass::Array)
            {
                if (!JS::GetArrayLength(cx_, obj, &length))
                {
                    return false;
                }
            }
            else
            {
                JS::RootedValue lengthValue(cx_);
                bool found = false;

                if (!GetDataProperty(cx_, obj, "length", &lengthValue, &found))
                {
                    return false;
                }

                if (found && lengthValue.isInt32() && lengthValue.toInt32() > 0)
                {
                    length = static_cast<uint32_t>(lengthValue.toInt32());
                }
            }

            return RenderArrayLike(obj, length,
                                   cls == js::ESClass::Arguments ? StringView("[Arguments] ")
                                                                 : StringView(),
                                   false, depth, indent, out);
        }
        case js::ESClass::Map:
            return RenderMapOrSet(obj, true, depth, indent, out);
        case js::ESClass::Set:
            return RenderMapOrSet(obj, false, depth, indent, out);
        case js::ESClass::Promise:
            return RenderPromise(obj, depth, indent, out);
        default:
            break;
        }

        if (JS_IsTypedArrayObject(obj))
        {
            const size_t length = JS_GetTypedArrayLength(obj);
            String prefix;
            Text text(prefix);
            text.Add(JS::GetClass(obj)->name);
            text.Add('(');
            text.AddUnsigned(length);
            text.Add(") ");

            if (!text.Ok())
            {
                out.Fail();
                return true;
            }

            const uint32_t clamped =
                length > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(length);
            return RenderArrayLike(obj, clamped, prefix.View(), true, depth, indent, out);
        }

        if (JS::IsCallable(obj))
        {
            return RenderFunction(obj, depth, indent, out);
        }

        String prefix;
        Text prefixText(prefix);

        if (!AppendConstructorPrefix(obj, prefixText))
        {
            return false;
        }

        if (!prefixText.Ok())
        {
            out.Fail();
            return true;
        }

        forge::core::Vector<String> entries;

        if (!CollectOwnProperties(obj, false, nullptr, depth, indent, entries, out))
        {
            return false;
        }

        Join(prefix.View(), "{", "}", entries, indent, out);
        return true;
    }

    JSContext* cx_;
    InspectOptions options_;
    JS::RootedVector<JSObject*> seen_;
};

/// %d / %i: an integer, or NaN for anything that is not a number or BigInt.
bool AppendInteger(
    JSContext* cx,
    JS::HandleValue value,
    Text& out)
{
    if (value.isBigInt())
    {
        JS::RootedString str(cx, JS::ToString(cx, value));

        if (!str || !AppendJSString(cx, str, out))
        {
            return false;
        }

        out.Add('n');
        return true;
    }

    if (!value.isNumber())
    {
        out.Add("NaN");
        return true;
    }

    const double number = trunc(value.toNumber());
    JS::RootedValue truncated(cx, JS::NumberValue(number == 0 ? 0.0 : number));
    JS::RootedString str(cx, JS::ToString(cx, truncated));
    return str && AppendJSString(cx, str, out);
}

/// %f: a number, or NaN for anything else.
bool AppendFloat(
    JSContext* cx,
    JS::HandleValue value,
    Text& out)
{
    if (!value.isNumber())
    {
        out.Add("NaN");
        return true;
    }

    JS::RootedString str(cx, JS::ToString(cx, value));
    return str && AppendJSString(cx, str, out);
}

} // namespace

bool AppendStringUtf8(
    JSContext* cx,
    JSString* str,
    forge::core::String& out)
{
    Text text(out);

    if (!AppendJSString(cx, str, text))
    {
        return false;
    }

    if (!text.Ok())
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

bool AppendInspected(
    JSContext* cx,
    JS::HandleValue value,
    forge::core::String& out,
    const InspectOptions& options)
{
    Text text(out);
    Inspector inspector(cx, options);

    if (!inspector.Render(value, 0, 0, text))
    {
        return false;
    }

    return text.Ok();
}

bool AppendFormattedArgs(
    JSContext* cx,
    const JS::CallArgs& args,
    unsigned first,
    forge::core::String& out)
{
    Text text(out);
    unsigned next = first;

    if (next < args.length() && args[next].isString())
    {
        String format;
        Text formatText(format);

        if (!AppendJSString(cx, args[next].toString(), formatText))
        {
            return false;
        }

        if (!formatText.Ok())
        {
            return false;
        }

        ++next;
        const StringView view = format.View();
        Size start = 0;

        for (Size i = 0; i < view.Size(); ++i)
        {
            if (view.Data()[i] != '%' || i + 1 >= view.Size())
            {
                continue;
            }

            const char spec = view.Data()[i + 1];
            const bool known = spec == 's' || spec == 'd' || spec == 'i' || spec == 'f' ||
                               spec == 'o' || spec == 'O' || spec == 'c' || spec == '%';

            if (!known)
            {
                continue;
            }

            text.Add(StringView(view.Data() + start, i - start));
            start = i + 2;
            ++i;

            if (spec == '%')
            {
                text.Add('%');
                continue;
            }

            if (next >= args.length())
            {
                // No argument left: the specifier stays as written.
                text.Add('%');
                text.Add(spec);
                continue;
            }

            JS::RootedValue arg(cx, args[next++]);
            bool ok = true;

            switch (spec)
            {
            case 's':
            {
                InspectOptions options;
                options.depth = 0;
                Inspector inspector(cx, options);
                ok = inspector.Render(arg, 0, 0, text);
                break;
            }
            case 'd':
            case 'i':
                ok = AppendInteger(cx, arg, text);
                break;
            case 'f':
                ok = AppendFloat(cx, arg, text);
                break;
            case 'o':
            case 'O':
            {
                InspectOptions options;
                options.rawTopLevelStrings = false;
                Inspector inspector(cx, options);
                ok = inspector.Render(arg, 0, 0, text);
                break;
            }
            case 'c':
                break;  // CSS styling: consumed, not shown
            default:
                break;
            }

            if (!ok)
            {
                return false;
            }
        }

        text.Add(StringView(view.Data() + start, view.Size() - start));
    }

    for (; next < args.length(); ++next)
    {
        if (next > first)
        {
            text.Add(' ');
        }

        Inspector inspector(cx, InspectOptions());

        if (!inspector.Render(args[next], 0, 0, text))
        {
            return false;
        }
    }

    return text.Ok();
}

} // namespace forge::runtime
