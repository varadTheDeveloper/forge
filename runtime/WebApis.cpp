// Web-standard APIs (Phase 10.1). See WebApis.h and WebApis.js.

#include "runtime/WebApis.h"

#include <string.h>

#include <chrono>
#include <utility>

#include "js/Array.h"
#include "js/ArrayBuffer.h"
#include "js/CallAndConstruct.h"
#include "js/CallArgs.h"
#include "js/CompilationAndEvaluation.h"
#include "js/CompileOptions.h"
#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/Object.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/Promise.h"
#include "js/SavedFrameAPI.h"
#include "js/SourceText.h"
#include "js/Stack.h"
#include "js/String.h"
#include "js/WeakMap.h"
#include "js/Wrapper.h"
#include "js/experimental/TypedData.h"
#include "jsapi.h"
#include "jsfriendapi.h"
#include "mozilla/Utf8.h"

#include "forge-core/String.h"
#include "forge-core/memory/MakeUnique.h"
#include "forge-core/memory/UniquePtr.h"
#include "forge-core/memory/Vector.h"
#include "runtime/Encoding.h"
#include "runtime/JsUtil.h"

namespace forge::runtime
{

const char* const kWebApiGlobalNames[] = {
    // Phase 10.1
    "DOMException",
    "TextDecoder",
    "TextEncoder",
    "atob",
    "btoa",
    // Phase 10.2
    "CustomEvent",
    "ErrorEvent",
    "Event",
    "EventTarget",
    "PromiseRejectionEvent",
    "navigator",
    "onerror",
    "onrejectionhandled",
    "onunhandledrejection",
    "reportError",
    "self",
    // Phase 10.3
    "AbortController",
    "AbortSignal",
};

const forge::core::Size kWebApiGlobalCount = sizeof(kWebApiGlobalNames) / sizeof(kWebApiGlobalNames[0]);

namespace
{

using forge::core::Size;
using forge::core::String;
using forge::core::u8;
using forge::core::Vector;
namespace encoding = forge::runtime::encoding;

#include "runtime/WebApis.js.inc"

/// The source name of WebApis.js. Error stacks skip frames of every source
/// whose name starts with "forge:" (see MakeError).
constexpr const char kWebApisSourceName[] = "forge:web";

/// The most arguments a forwarded call passes on (no web API method takes
/// more than a few).
constexpr uint32_t kMaxForwardedArguments = 16;

/// Phase 10.2: the program's global object (DefineWebApis with
/// WebRealm::Program), where uncaught errors and rejections are dispatched.
JS::PersistentRootedObject* g_programGlobal = nullptr;

/// Set while an "error" event is being dispatched: an exception thrown by
/// one of its listeners is then uncaught at once (no second event), as in
/// the HTML Standard's "report an exception".
bool g_reportingException = false;

/// The time origin of Event.timeStamp (and, in Phase 10.6, performance).
std::chrono::steady_clock::time_point g_timeOrigin;
bool g_timeOriginSet = false;

/// A native called with arguments WebApis.js never passes: a bug in Forge,
/// reported as a TypeError rather than a crash.
bool Misuse(JSContext* cx, const char* native)
{
    JS_ReportErrorASCII(cx, "Forge: internal web API helper %s was called incorrectly", native);
    return false;
}

/// Copies the code units of `str` into `out`.
bool CopyUnits(JSContext* cx, JS::HandleString str, Vector<char16_t>& out)
{
    const size_t length = JS_GetStringLength(str);
    out.Clear();

    if (out.Reserve(static_cast<Size>(length)).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    for (size_t i = 0; i < length; ++i)
    {
        if (out.PushBack(char16_t(0)).HasError())
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }
    }

    return length == 0 || JS_CopyStringChars(cx, mozilla::Range<char16_t>(out.Data(), length), str);
}

JSString* NewString(JSContext* cx, const Vector<char16_t>& units)
{
    return units.Size() == 0 ? JS_GetEmptyString(cx)
                             : JS_NewUCStringCopyN(cx, units.Data(), static_cast<size_t>(units.Size()));
}

/// A string of one character per byte (Latin-1), as atob returns.
JSString* NewByteString(JSContext* cx, const char* bytes, Size count)
{
    return count == 0 ? JS_GetEmptyString(cx) : JS_NewStringCopyN(cx, bytes, static_cast<size_t>(count));
}

/// The bytes of an ArrayBuffer or ArrayBufferView, or of a wrapper of one.
/// A detached buffer has none. False if `obj` is neither. The pointer is
/// valid only until the next call that can run script or GC.
bool BufferSourceBytes(JSObject* obj, const u8** data, size_t* length)
{
    bool isSharedMemory = false;
    uint8_t* bytes = nullptr;
    size_t count = 0;

    if (JS_GetObjectAsArrayBufferView(obj, &count, &isSharedMemory, &bytes) ||
        JS::GetObjectAsArrayBuffer(obj, &count, &bytes))
    {
        *data = bytes;
        *length = bytes != nullptr ? count : 0;
        return true;
    }

    return false;
}

/// The realm's internals object (null if `global` is not one of Forge's
/// globals or DefineWebApis has not run in it).
JSObject* InternalsOf(JSObject* global)
{
    if (global == nullptr || JSCLASS_RESERVED_SLOTS(JS::GetClass(global)) <= kWebInternalsSlot)
    {
        return nullptr;
    }

    const JS::Value value = JS::GetReservedSlot(global, kWebInternalsSlot);
    return value.isObject() ? &value.toObject() : nullptr;
}

/// The value of `obj`'s own data property `name`, or undefined. Runs no
/// script (the objects read here are plain objects WebApis.js made).
bool GetOwnData(JSContext* cx, JS::HandleObject obj, const char* name, JS::MutableHandleValue value)
{
    value.setUndefined();
    JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx);

    if (!JS_GetOwnPropertyDescriptor(cx, obj, name, &desc))
    {
        return false;
    }

    if (desc.isSome() && desc->isDataDescriptor())
    {
        value.set(desc->value());
    }

    return true;
}

bool IsForgeSource(JSContext* cx, JS::HandleString source, bool* result)
{
    static constexpr char kPrefix[] = "forge:";
    constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;
    *result = false;

    if (JS_GetStringLength(source) < kPrefixLength)
    {
        return true;
    }

    for (size_t i = 0; i < kPrefixLength; ++i)
    {
        char16_t c = 0;

        if (!JS_GetStringCharAt(cx, source, i, &c))
        {
            return false;
        }

        if (c != static_cast<char16_t>(kPrefix[i]))
        {
            return true;
        }
    }

    *result = true;
    return true;
}

/// The current stack from its first frame outside Forge's own scripts
/// (source names starting with "forge:"), with that frame's file, line and
/// column. `stack` may end up null (no such frame).
bool CaptureUserStack(JSContext* cx, JS::MutableHandleObject stack, JS::MutableHandleString fileName,
                      uint32_t* line, JS::TaggedColumnNumberOneOrigin* column)
{
    if (!JS::CaptureCurrentStack(cx, stack))
    {
        return false;
    }

    fileName.set(JS_GetEmptyString(cx));
    *line = 0;
    JS::RootedString source(cx);
    JS::RootedObject parent(cx);

    while (stack)
    {
        if (JS::GetSavedFrameSource(cx, nullptr, stack, &source) != JS::SavedFrameResult::Ok ||
            !source)
        {
            break;
        }

        bool internal = false;

        if (!IsForgeSource(cx, source, &internal))
        {
            return false;
        }

        if (!internal)
        {
            fileName.set(source);
            (void)JS::GetSavedFrameLine(cx, nullptr, stack, line);
            (void)JS::GetSavedFrameColumn(cx, nullptr, stack, column);
            break;
        }

        if (JS::GetSavedFrameParent(cx, nullptr, stack, &parent) != JS::SavedFrameResult::Ok)
        {
            parent = nullptr;
        }

        if (!parent &&
            JS::GetSavedFrameAsyncParent(cx, nullptr, stack, &parent) != JS::SavedFrameResult::Ok)
        {
            parent = nullptr;
        }

        stack.set(parent);
    }

    return true;
}

/// A new error of `type` whose stack, file name, line and column come from
/// the first frame outside Forge's own scripts, so an error a web API throws
/// points at the code that called it. With a null `message` it has no own
/// `message` property (DOMException keeps its message internally).
bool MakeError(JSContext* cx, JSExnType type, JS::HandleString message, JS::MutableHandleValue error)
{
    JS::RootedObject stack(cx);
    JS::RootedString fileName(cx);
    uint32_t line = 0;
    JS::TaggedColumnNumberOneOrigin column;

    if (!CaptureUserStack(cx, &stack, &fileName, &line, &column))
    {
        return false;
    }

    JS::Rooted<mozilla::Maybe<JS::Value>> cause(cx, mozilla::Nothing());
    return JS::CreateError(cx, type, stack, fileName, line,
                           JS::ColumnNumberOneOrigin(column.oneOriginValue()), nullptr, message,
                           cause, error);
}

/// Throws a TypeError made by MakeError. Always false.
bool ThrowWebTypeError(JSContext* cx, const char* message)
{
    JS::RootedString text(cx, JS_NewStringCopyZ(cx, message));
    JS::RootedValue error(cx);

    if (text && MakeError(cx, JSEXN_TYPEERR, text, &error))
    {
        JS_SetPendingException(cx, error);
    }

    return false;
}

//------------------------------------------------------------------------------
// The natives WebApis.js receives
//------------------------------------------------------------------------------

/// makeError(kind, message | undefined, prototype | undefined)
bool NativeMakeError(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 3 || !args[0].isInt32())
    {
        return Misuse(cx, "makeError");
    }

    const int32_t kind = args[0].toInt32();
    const JSExnType type = kind == 1 ? JSEXN_TYPEERR : kind == 2 ? JSEXN_RANGEERR : JSEXN_ERR;
    JS::RootedString message(cx, args[1].isString() ? args[1].toString() : nullptr);
    JS::RootedValue error(cx);

    if (!MakeError(cx, type, message, &error))
    {
        return false;
    }

    if (args[2].isObject())
    {
        JS::RootedObject obj(cx, &error.toObject());
        JS::RootedObject proto(cx, &args[2].toObject());

        if (!JS_SetPrototype(cx, obj, proto))
        {
            return false;
        }
    }

    args.rval().set(error);
    return true;
}

/// forward(name, self, argumentsObject, box): if `self` is a wrapper of an
/// object of another compartment, calls that compartment's member `name`
/// ("Interface.member", from its internals) on the object itself, with the
/// arguments wrapped into that compartment, puts the result (wrapped back)
/// in box.value and returns true. Otherwise returns false. Arguments and
/// results cross through the ordinary wrapper policy, so a forwarded call
/// can pass nothing an ordinary call through the wrapper could not.
bool NativeForward(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 4 || !args[0].isString() || !args[2].isObject() || !args[3].isObject())
    {
        return Misuse(cx, "forward");
    }

    args.rval().setBoolean(false);

    if (!args[1].isObject() || !js::IsCrossCompartmentWrapper(&args[1].toObject()))
    {
        return true;
    }

    JS::RootedObject target(cx, js::UncheckedUnwrap(&args[1].toObject()));

    if (!target || JS_IsDeadWrapper(target) || js::IsCrossCompartmentWrapper(target))
    {
        return true;
    }

    // The arguments, read in this realm from the arguments object the
    // script passed.
    JS::RootedObject argumentsObject(cx, &args[2].toObject());
    JS::RootedValue lengthValue(cx);

    if (!JS_GetProperty(cx, argumentsObject, "length", &lengthValue))
    {
        return false;
    }

    uint32_t count = lengthValue.isInt32() && lengthValue.toInt32() > 0
                         ? static_cast<uint32_t>(lengthValue.toInt32())
                         : 0;
    count = count < kMaxForwardedArguments ? count : kMaxForwardedArguments;
    JS::RootedValueVector values(cx);
    JS::RootedValue item(cx);

    for (uint32_t i = 0; i < count; ++i)
    {
        if (!JS_GetElement(cx, argumentsObject, i, &item) || !values.append(item))
        {
            return false;
        }
    }

    JS::RootedValue name(cx, args[0]);
    JS::RootedValue result(cx);
    JS::RootedValue exception(cx);
    bool called = false;
    bool ok = false;
    bool thrown = false;

    {
        JSAutoRealm ar(cx, target);
        // A realm without internals (or without the member) is simply not a
        // place to forward to: `called` stays false and nothing is thrown.
        JS::RootedObject internals(cx, InternalsOf(JS::CurrentGlobalOrNull(cx)));
        JS::RootedValue membersValue(cx);
        JS::RootedValue member(cx);
        JS::RootedId id(cx);

        ok = !internals || GetOwnData(cx, internals, "members", &membersValue);

        if (ok && membersValue.isObject())
        {
            JS::RootedObject members(cx, &membersValue.toObject());
            JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx);
            ok = JS_WrapValue(cx, &name) && JS_ValueToId(cx, name, &id) &&
                 JS_GetOwnPropertyDescriptorById(cx, members, id, &desc);

            if (ok && desc.isSome() && desc->isDataDescriptor() && desc->value().isObject() &&
                JS::IsCallable(&desc->value().toObject()))
            {
                member = desc->value();
            }
        }

        if (ok && member.isObject())
        {
            called = true;

            for (size_t i = 0; ok && i < values.length(); ++i)
            {
                ok = JS_WrapValue(cx, values[i]);
            }

            JS::RootedValue self(cx, JS::ObjectValue(*target));
            ok = ok && JS::Call(cx, self, member, JS::HandleValueArray(values), &result);
        }

        if (!ok)
        {
            thrown = TakePendingException(cx, &exception);
        }
    }

    if (!ok)
    {
        // Rethrow in this realm.
        if (thrown && JS_WrapValue(cx, &exception))
        {
            JS_SetPendingException(cx, exception);
        }

        return false;
    }

    if (!called)
    {
        return true;
    }

    JS::RootedObject box(cx, &args[3].toObject());

    if (!JS_WrapValue(cx, &result) || !JS_DefineProperty(cx, box, "value", result, JSPROP_ENUMERATE))
    {
        return false;
    }

    args.rval().setBoolean(true);
    return true;
}

/// encodingForLabel(label) -> encoding number, or -1 (not a label), -2 (a
/// label of "replacement"), -3 (an encoding Forge does not decode yet).
bool NativeEncodingForLabel(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 1 || !args[0].isString())
    {
        return Misuse(cx, "encodingForLabel");
    }

    JS::RootedString label(cx, args[0].toString());
    Vector<char16_t> units;

    if (!CopyUnits(cx, label, units))
    {
        return false;
    }

    Size found = 0;

    switch (encoding::FindEncoding(units.Data(), units.Size(), found))
    {
    case encoding::LabelLookup::Supported:
        args.rval().setInt32(static_cast<int32_t>(found));
        break;
    case encoding::LabelLookup::Replacement:
        args.rval().setInt32(-2);
        break;
    case encoding::LabelLookup::Unsupported:
        args.rval().setInt32(-3);
        break;
    case encoding::LabelLookup::Unknown:
        args.rval().setInt32(-1);
        break;
    }

    return true;
}

/// encodingName(encoding) -> "utf-8", "windows-1252", ...
bool NativeEncodingName(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 1 || !args[0].isInt32() || args[0].toInt32() < 0 ||
        static_cast<Size>(args[0].toInt32()) >= encoding::EncodingCount())
    {
        return Misuse(cx, "encodingName");
    }

    JSString* name = JS_NewStringCopyZ(cx, encoding::EncodingName(static_cast<Size>(args[0].toInt32())));

    if (!name)
    {
        return false;
    }

    args.rval().setString(name);
    return true;
}

/// isBufferSource(value) -> true for an ArrayBuffer or ArrayBufferView.
bool NativeIsBufferSource(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    const u8* data = nullptr;
    size_t length = 0;
    args.rval().setBoolean(args.length() >= 1 && args[0].isObject() &&
                           BufferSourceBytes(&args[0].toObject(), &data, &length));
    return true;
}

/// decode(encoding, pending | undefined, input | undefined, fatal, final)
///   -> [text, pending | undefined], or null when fatal decoding fails.
/// `pending` is the Uint8Array this native returned last time: the bytes of
/// an incomplete sequence, decoded before `input`.
bool NativeDecode(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 5 || !args[0].isInt32() || args[0].toInt32() < 0 ||
        static_cast<Size>(args[0].toInt32()) >= encoding::EncodingCount() ||
        !(args[1].isUndefined() || args[1].isObject()) ||
        !(args[2].isUndefined() || args[2].isObject()) || !args[3].isBoolean() ||
        !args[4].isBoolean())
    {
        return Misuse(cx, "decode");
    }

    const Size which = static_cast<Size>(args[0].toInt32());
    const bool fatal = args[3].toBoolean();
    const bool final = args[4].toBoolean();
    Vector<char16_t> text;
    Vector<u8> combined;
    Vector<u8> rest;
    encoding::Status status = encoding::Status::Ok;
    bool badInput = false;
    bool outOfMemory = false;

    {
        JS::AutoCheckCannotGC nogc(cx);
        const u8* pendingData = nullptr;
        size_t pendingLength = 0;
        const u8* inputData = nullptr;
        size_t inputLength = 0;

        if ((args[1].isObject() && !BufferSourceBytes(&args[1].toObject(), &pendingData, &pendingLength)) ||
            (args[2].isObject() && !BufferSourceBytes(&args[2].toObject(), &inputData, &inputLength)))
        {
            badInput = true;
        }
        else
        {
            const u8* data = inputData;
            size_t length = inputLength;

            if (pendingLength > 0)
            {
                outOfMemory = combined.Reserve(static_cast<Size>(pendingLength + inputLength)).HasError();

                for (size_t i = 0; !outOfMemory && i < pendingLength; ++i)
                {
                    outOfMemory = combined.PushBack(pendingData[i]).HasError();
                }

                for (size_t i = 0; !outOfMemory && i < inputLength; ++i)
                {
                    outOfMemory = combined.PushBack(inputData[i]).HasError();
                }

                data = combined.Data();
                length = static_cast<size_t>(combined.Size());
            }

            Size consumed = 0;

            if (!outOfMemory)
            {
                status = encoding::Decode(which, data, static_cast<Size>(length), fatal, final, text,
                                          consumed);
            }

            for (Size i = consumed; !outOfMemory && status == encoding::Status::Ok && i < length; ++i)
            {
                outOfMemory = rest.PushBack(data[i]).HasError();
            }
        }
    }

    if (badInput)
    {
        return Misuse(cx, "decode");
    }

    if (outOfMemory || status == encoding::Status::OutOfMemory)
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    if (status == encoding::Status::Invalid)
    {
        args.rval().setNull();
        return true;
    }

    JS::RootedValueArray<2> pair(cx);
    JSString* string = NewString(cx, text);

    if (!string)
    {
        return false;
    }

    pair[0].setString(string);

    if (rest.Size() > 0)
    {
        JSObject* pending = Uint8ArrayFromBytes(cx, std::move(rest));

        if (!pending)
        {
            return false;
        }

        pair[1].setObject(*pending);
    }

    JSObject* array = JS::NewArrayObject(cx, pair);

    if (!array)
    {
        return false;
    }

    args.rval().setObject(*array);
    return true;
}

/// utf8Encode(string) -> Uint8Array (lone surrogates become U+FFFD).
bool NativeUtf8Encode(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 1 || !args[0].isString())
    {
        return Misuse(cx, "utf8Encode");
    }

    JS::RootedString str(cx, args[0].toString());
    Vector<char16_t> units;
    Vector<u8> bytes;

    if (!CopyUnits(cx, str, units))
    {
        return false;
    }

    if (!encoding::EncodeUtf8(units.Data(), units.Size(), bytes))
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    JSObject* array = Uint8ArrayFromBytes(cx, std::move(bytes));

    if (!array)
    {
        return false;
    }

    args.rval().setObject(*array);
    return true;
}

/// utf8EncodeInto(string, destination) -> { read, written }. Throws a
/// TypeError unless `destination` is a Uint8Array.
bool NativeUtf8EncodeInto(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 2 || !args[0].isString())
    {
        return Misuse(cx, "utf8EncodeInto");
    }

    size_t capacity = 0;
    bool isSharedMemory = false;
    uint8_t* data = nullptr;

    if (!args[1].isObject() ||
        !JS_GetObjectAsUint8Array(&args[1].toObject(), &capacity, &isSharedMemory, &data))
    {
        return ThrowWebTypeError(cx, "TextEncoder.encodeInto: the destination is not a Uint8Array.");
    }

    // At most `capacity` code units fit (each needs a byte or more), plus one
    // to see whether a lead surrogate at the edge has its trail.
    JS::RootedString str(cx, args[0].toString());
    const size_t length = JS_GetStringLength(str);
    const size_t take = length <= capacity ? length : capacity + 1;
    JS::RootedString prefix(cx, take == length ? str.get() : JS_NewDependentString(cx, str, 0, take));
    Vector<char16_t> units;

    if (!prefix || !CopyUnits(cx, prefix, units))
    {
        return false;
    }

    Size read = 0;
    Size written = 0;

    {
        // The copy above can GC and move the buffer's data: look again.
        JS::AutoCheckCannotGC nogc(cx);

        if (JS_GetObjectAsUint8Array(&args[1].toObject(), &capacity, &isSharedMemory, &data) &&
            data != nullptr)
        {
            encoding::EncodeUtf8Into(units.Data(), units.Size(), data, static_cast<Size>(capacity),
                                     read, written);
        }
    }

    JS::RootedObject result(cx, JS_NewPlainObject(cx));
    JS::RootedValue readValue(cx, JS::NumberValue(static_cast<double>(read)));
    JS::RootedValue writtenValue(cx, JS::NumberValue(static_cast<double>(written)));

    if (!result || !JS_DefineProperty(cx, result, "read", readValue, JSPROP_ENUMERATE) ||
        !JS_DefineProperty(cx, result, "written", writtenValue, JSPROP_ENUMERATE))
    {
        return false;
    }

    args.rval().setObject(*result);
    return true;
}

/// atob(string) -> byte string, or null if `string` is not base64.
bool NativeAtob(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 1 || !args[0].isString())
    {
        return Misuse(cx, "atob");
    }

    JS::RootedString str(cx, args[0].toString());
    Vector<char16_t> units;
    Vector<u8> bytes;

    if (!CopyUnits(cx, str, units))
    {
        return false;
    }

    const encoding::Status status = encoding::DecodeBase64(units.Data(), units.Size(), bytes);

    if (status == encoding::Status::OutOfMemory)
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    if (status == encoding::Status::Invalid)
    {
        args.rval().setNull();
        return true;
    }

    JSString* result = NewByteString(cx, reinterpret_cast<const char*>(bytes.Data()), bytes.Size());

    if (!result)
    {
        return false;
    }

    args.rval().setString(result);
    return true;
}

/// btoa(string) -> base64, or null if a code unit is above U+00FF.
bool NativeBtoa(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 1 || !args[0].isString())
    {
        return Misuse(cx, "btoa");
    }

    JS::RootedString str(cx, args[0].toString());
    Vector<char16_t> units;
    Vector<char> chars;

    if (!CopyUnits(cx, str, units))
    {
        return false;
    }

    const encoding::Status status = encoding::EncodeBase64(units.Data(), units.Size(), chars);

    if (status == encoding::Status::OutOfMemory)
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    if (status == encoding::Status::Invalid)
    {
        args.rval().setNull();
        return true;
    }

    JSString* result = NewByteString(cx, chars.Data(), chars.Size());

    if (!result)
    {
        return false;
    }

    args.rval().setString(result);
    return true;
}

/// now() -> milliseconds since the time origin, in steps of 0.1 ms. A
/// coarse clock makes timing side channels harder (Spectre); browsers
/// that are not cross-origin isolated use the same step.
bool NativeNow(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    const auto elapsed = std::chrono::steady_clock::now() - g_timeOrigin;
    const long long tenthsOfMs =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 100;
    args.rval().setDouble(static_cast<double>(tenthsOfMs) / 10.0);
    (void)cx;
    return true;
}

/// reportException(value, kind): an event listener threw `value` (kind 0),
/// or the program called reportError(value) (kind 1). Reports it (see
/// ReportException); never throws.
bool NativeReportException(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 2 || !args[1].isInt32())
    {
        return Misuse(cx, "reportException");
    }

    // The value's own stack is used when it is an Error; for any other value
    // the stack is the caller's (reportError's caller, or the dispatch).
    JS::RootedObject stack(cx);
    JS::RootedString fileName(cx);
    uint32_t line = 0;
    JS::TaggedColumnNumberOneOrigin column;

    if (!CaptureUserStack(cx, &stack, &fileName, &line, &column))
    {
        JS_ClearPendingException(cx);
        stack = nullptr;
    }

    JS::ExceptionStack exception(cx, args[0], stack);
    ReportException(cx, exception, args[1].toInt32() == 1 ? "reportError" : "an event listener");
    args.rval().setUndefined();
    return true;
}

/// scheduleTimeout(callback, milliseconds): AbortSignal.timeout's timer
/// (HostScheduleInternalTimer). `milliseconds` is an integer from 0 to
/// 2^53 - 1, already converted by the script.
bool NativeScheduleTimeout(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 2 || !args[0].isObject() || !JS::IsCallable(&args[0].toObject()) ||
        !args[1].isNumber())
    {
        return Misuse(cx, "scheduleTimeout");
    }

    JS::RootedObject callback(cx, &args[0].toObject());

    if (!HostScheduleInternalTimer(cx, callback, args[1].toNumber()))
    {
        return false;
    }

    args.rval().setUndefined();
    return true;
}

const JSFunctionSpec kNatives[] = {
    JS_FN("makeError", NativeMakeError, 3, 0),
    JS_FN("forward", NativeForward, 4, 0),
    JS_FN("encodingForLabel", NativeEncodingForLabel, 1, 0),
    JS_FN("encodingName", NativeEncodingName, 1, 0),
    JS_FN("isBufferSource", NativeIsBufferSource, 1, 0),
    JS_FN("decode", NativeDecode, 5, 0),
    JS_FN("utf8Encode", NativeUtf8Encode, 1, 0),
    JS_FN("utf8EncodeInto", NativeUtf8EncodeInto, 2, 0),
    JS_FN("atob", NativeAtob, 1, 0),
    JS_FN("btoa", NativeBtoa, 1, 0),
    JS_FN("now", NativeNow, 0, 0),
    JS_FN("reportException", NativeReportException, 2, 0),
    JS_FN("scheduleTimeout", NativeScheduleTimeout, 2, 0),
    JS_FS_END,
};

bool CountOwnKeys(JSContext* cx, JS::HandleObject obj, size_t* count)
{
    JS::RootedIdVector keys(cx);

    if (!js::GetPropertyKeys(cx, obj, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &keys))
    {
        return false;
    }

    *count = keys.length();
    return true;
}

/// Calls internals.<name>(argv) of the program's realm, which the caller
/// has entered. False (with an exception pending, or none after an
/// uncatchable error) if it could not be called or threw.
bool CallProgramInternal(JSContext* cx, const char* name, const JS::HandleValueArray& argv,
                         JS::MutableHandleValue rval)
{
    JS::RootedObject internals(cx, InternalsOf(JS::CurrentGlobalOrNull(cx)));
    JS::RootedValue function(cx);

    if (!internals || !GetOwnData(cx, internals, name, &function) || !function.isObject())
    {
        if (!JS_IsExceptionPending(cx))
        {
            JS_ReportErrorASCII(cx, "Forge: the web API internal %s is missing", name);
        }

        return false;
    }

    return JS::Call(cx, JS::UndefinedHandleValue, function, argv, rval);
}

JSString* NewUtf8String(JSContext* cx, const char* text)
{
    return text != nullptr ? JS_NewStringCopyUTF8Z(cx, JS::ConstUTF8CharsZ(text, strlen(text)))
                           : JS_GetEmptyString(cx);
}

} // namespace

bool DefineWebApis(JSContext* cx, JS::HandleObject global, WebRealm kind)
{
    if (kind == WebRealm::Program)
    {
        if (g_programGlobal != nullptr)
        {
            JS_ReportErrorASCII(cx, "Forge: the program's web APIs were defined twice");
            return false;
        }

        forge::core::Result<forge::core::memory::UniquePtr<JS::PersistentRootedObject>> made =
            forge::core::memory::MakeUnique<JS::PersistentRootedObject>(cx, global);

        if (made.HasError())
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }

        g_programGlobal = made.Value().Release();
        g_timeOrigin = std::chrono::steady_clock::now();
        g_timeOriginSet = true;
    }
    else if (g_programGlobal == nullptr || !g_timeOriginSet)
    {
        JS_ReportErrorASCII(cx, "Forge: a package's web APIs were defined before the program's");
        return false;
    }

    if (JSCLASS_RESERVED_SLOTS(JS::GetClass(global)) <= kWebInternalsSlot)
    {
        JS_ReportErrorASCII(cx, "Forge: the global object has no slot for the web APIs");
        return false;
    }

    JS::RootedObject internals(cx, JS_NewObjectWithGivenProto(cx, nullptr, nullptr));
    JS::RootedObject natives(cx, JS_NewObjectWithGivenProto(cx, nullptr, nullptr));

    if (!internals || !natives || !JS_DefineFunctions(cx, natives, kNatives))
    {
        return false;
    }

    JS::SetReservedSlot(global, kWebInternalsSlot, JS::ObjectValue(*internals));

    String source;

    for (const char* piece : kWebApisSourcePieces)
    {
        if (source.Append(piece).HasError())
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }
    }

    size_t keysBefore = 0;

    if (!CountOwnKeys(cx, global, &keysBefore))
    {
        return false;
    }

    JS::CompileOptions options(cx);
    options.setFileAndLine(kWebApisSourceName, 1);
    JS::SourceText<mozilla::Utf8Unit> text;
    JS::RootedValue setUp(cx);

    if (!text.init(cx, source.Data(), source.Size(), JS::SourceOwnership::Borrowed) ||
        !JS::Evaluate(cx, options, text, &setUp))
    {
        return false;
    }

    if (!setUp.isObject() || !JS::IsCallable(&setUp.toObject()))
    {
        JS_ReportErrorASCII(cx, "Forge: the web API script did not produce a function");
        return false;
    }

    JS::RootedValueArray<3> argv(cx);
    argv[0].setObject(*natives);
    argv[1].setObject(*global);
    argv[2].setObject(*internals);
    JS::RootedValue ignored(cx);

    if (!JS::Call(cx, JS::UndefinedHandleValue, setUp, argv, &ignored))
    {
        return false;
    }

    // Rule 4 of WebApis.js: exactly the listed globals were added.
    size_t keysAfter = 0;

    if (!CountOwnKeys(cx, global, &keysAfter))
    {
        return false;
    }

    bool allPresent = keysAfter == keysBefore + kWebApiGlobalCount;

    for (Size i = 0; allPresent && i < kWebApiGlobalCount; ++i)
    {
        if (!JS_HasOwnProperty(cx, global, kWebApiGlobalNames[i], &allPresent))
        {
            return false;
        }
    }

    if (!allPresent)
    {
        JS_ReportErrorASCII(cx, "Forge: runtime/WebApis.js and kWebApiGlobalNames disagree");
        return false;
    }

    return true;
}

bool GetDomExceptionFields(JSContext* cx, JS::HandleObject obj, JS::MutableHandleValue name,
                           JS::MutableHandleValue message, bool* found)
{
    *found = false;
    JS::RootedObject internals(cx, InternalsOf(JS::CurrentGlobalOrNull(cx)));
    JS::RootedValue mapValue(cx);

    if (!internals || !GetOwnData(cx, internals, "domExceptions", &mapValue) || !mapValue.isObject())
    {
        return !JS_IsExceptionPending(cx);
    }

    JS::RootedObject map(cx, &mapValue.toObject());
    JS::RootedValue key(cx, JS::ObjectValue(*obj));
    JS::RootedValue recordValue(cx);

    if (!JS::GetWeakMapEntry(cx, map, key, &recordValue))
    {
        return false;
    }

    if (!recordValue.isObject())
    {
        return true;
    }

    JS::RootedObject record(cx, &recordValue.toObject());

    if (!GetOwnData(cx, record, "name", name) || !GetOwnData(cx, record, "message", message))
    {
        return false;
    }

    *found = true;
    return true;
}

void ShutdownWebApis()
{
    delete g_programGlobal;
    g_programGlobal = nullptr;
}

void ReportException(JSContext* cx, const JS::ExceptionStack& exception, const char* context)
{
    if (g_programGlobal == nullptr || g_reportingException)
    {
        HostReportUncaught(cx, exception, context);
        return;
    }

    // The ErrorEvent's fields, from the error report: it runs no script (no
    // getter, no toString), so reporting cannot be hijacked by the value.
    String message;
    String fileName;
    uint32_t line = 0;
    uint32_t column = 0;

    {
        JS::ErrorReportBuilder report(cx);

        if (report.init(cx, exception, JS::ErrorReportBuilder::NoSideEffects))
        {
            if (report.toStringResult())
            {
                message.Append(report.toStringResult().c_str()).Ignore();
            }

            if (const JSErrorReport* r = report.report())
            {
                if (r->filename)
                {
                    fileName.Append(r->filename.c_str()).Ignore();
                }

                line = r->lineno;
                column = r->column.oneOriginValue();
            }
        }

        JS_ClearPendingException(cx);
    }

    bool notCanceled = true;
    g_reportingException = true;

    {
        JS::RootedObject programGlobal(cx, *g_programGlobal);
        JSAutoRealm ar(cx, programGlobal);
        JS::RootedValueArray<5> argv(cx);
        argv[0].set(exception.exception());
        JSString* messageString = NewUtf8String(cx, message.CStr());
        JSString* fileString = messageString ? NewUtf8String(cx, fileName.CStr()) : nullptr;
        bool ok = messageString && fileString;

        if (ok)
        {
            argv[1].setString(messageString);
            argv[2].setString(fileString);
            argv[3].setNumber(line);
            argv[4].setNumber(column);
            JS::RootedValue rval(cx);
            ok = JS_WrapValue(cx, argv[0]) && CallProgramInternal(cx, "dispatchError", argv, &rval);
            notCanceled = !ok || rval.isTrue();
        }

        JS_ClearPendingException(cx);
    }

    g_reportingException = false;

    if (notCanceled)
    {
        HostReportUncaught(cx, exception, context);
    }
}

void FireUnhandledRejection(JSContext* cx, JS::HandleObject promise, bool* handled)
{
    *handled = false;

    if (g_programGlobal == nullptr)
    {
        return;
    }

    bool canceled = false;

    {
        JS::RootedValueArray<2> argv(cx);
        argv[0].setObject(*promise);
        argv[1].set(JS::GetPromiseResult(promise));
        JS::RootedObject programGlobal(cx, *g_programGlobal);
        JSAutoRealm ar(cx, programGlobal);
        JS::RootedValue rval(cx);
        canceled = JS_WrapValue(cx, argv[0]) && JS_WrapValue(cx, argv[1]) &&
                   CallProgramInternal(cx, "dispatchUnhandledRejection", argv, &rval) &&
                   rval.isFalse();
        JS_ClearPendingException(cx);
    }

    if (!canceled)
    {
        return;
    }

    *handled = true;

    // HTML: a rejection still without a handler after its event joins the
    // "outstanding rejected promises", which fire rejectionhandled later.
    if (!JS::GetPromiseIsHandled(promise))
    {
        JS::RootedValue promiseValue(cx, JS::ObjectValue(*promise));
        JS::RootedObject programGlobal(cx, *g_programGlobal);
        JSAutoRealm ar(cx, programGlobal);
        JS::RootedValue rval(cx);

        if (!JS_WrapValue(cx, &promiseValue) ||
            !CallProgramInternal(cx, "rememberRejection", JS::HandleValueArray(promiseValue), &rval))
        {
            JS_ClearPendingException(cx);
        }
    }
}

void FireRejectionHandled(JSContext* cx, JS::HandleObject promise)
{
    if (g_programGlobal == nullptr)
    {
        return;
    }

    JS::RootedValueArray<2> argv(cx);
    argv[0].setObject(*promise);
    argv[1].set(JS::GetPromiseResult(promise));
    JS::RootedObject programGlobal(cx, *g_programGlobal);
    JSAutoRealm ar(cx, programGlobal);
    JS::RootedValue rval(cx);

    if (!JS_WrapValue(cx, argv[0]) || !JS_WrapValue(cx, argv[1]) ||
        !CallProgramInternal(cx, "dispatchRejectionHandled", argv, &rval))
    {
        JS_ClearPendingException(cx);
    }
}

} // namespace forge::runtime
