// Shared JavaScript-binding helpers (Phase 8.3). These used to be static
// functions in forge.cpp; the filesystem natives moved to runtime/Fs.cpp and
// need them too. See JsUtil.h for the list.

#include "runtime/JsUtil.h"

#include <stdio.h>
#include <string.h>

#include <utility>

#include "js/ArrayBuffer.h"
#include "js/CharacterEncoding.h"
#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"
#include "js/String.h"
#include "js/experimental/TypedData.h"
#include "jsapi.h"

#include "runtime/Inspect.h"

namespace forge::runtime
{

// Custom TypeError / RangeError messages (Phase 8). JS_ReportErrorASCII can
// only throw a plain Error; the engine creates other error types from a
// message table like this one.
enum ForgeErrorNumber : unsigned {
  kForgeTypeError,
  kForgeRangeError,
  kForgeErrorCount
};

static const JSErrorFormatString kForgeErrorFormats[kForgeErrorCount] = {
    {"ForgeTypeError", "{0}", 1, JSEXN_TYPEERR},
    {"ForgeRangeError", "{0}", 1, JSEXN_RANGEERR},
};

static const JSErrorFormatString* GetForgeErrorMessage(void* userRef, const unsigned number) {
  (void)userRef;
  return number < kForgeErrorCount ? &kForgeErrorFormats[number] : nullptr;
}

bool ThrowTypeError(JSContext* cx, const char* message) {
  JS_ReportErrorNumberUTF8(cx, GetForgeErrorMessage, nullptr, kForgeTypeError, message);
  return false;
}

bool ThrowRangeError(JSContext* cx, const char* message) {
  JS_ReportErrorNumberUTF8(cx, GetForgeErrorMessage, nullptr, kForgeRangeError, message);
  return false;
}


// ErrorCode -> error.code string table from JsBindings.md's "Error
// Handling Policy". Exhaustive over every ErrorCode enumerator (verified
// against forge-core/Error.h directly) so adding a new enumerator there
// without updating this switch is a compiler error (no `default:`), not
// a silent "Unknown" fallback for a code that should have had a real
// mapping -- the trailing return below only covers the (impossible in
// practice) case of `code` holding a value outside the enum's defined
// range.
const char* ErrorCodeToString(forge::core::ErrorCode code) {
  using forge::core::ErrorCode;

  switch (code) {
    case ErrorCode::None:             return "None";
    case ErrorCode::Unknown:          return "Unknown";
    case ErrorCode::InvalidArgument:  return "InvalidArgument";
    case ErrorCode::InvalidOperation: return "InvalidOperation";
    case ErrorCode::NotSupported:     return "NotSupported";
    case ErrorCode::NotImplemented:   return "NotImplemented";
    case ErrorCode::NotFound:         return "NotFound";
    case ErrorCode::AlreadyExists:    return "AlreadyExists";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::Busy:             return "Busy";
    case ErrorCode::Timeout:          return "Timeout";
    case ErrorCode::Cancelled:        return "Cancelled";
    case ErrorCode::EndOfFile:        return "EndOfFile";
    case ErrorCode::IOError:          return "IOError";
    case ErrorCode::OutOfMemory:      return "OutOfMemory";
    case ErrorCode::BufferTooSmall:   return "BufferTooSmall";
    case ErrorCode::Overflow:         return "Overflow";
    case ErrorCode::Underflow:        return "Underflow";
    case ErrorCode::InvalidData:      return "InvalidData";
    case ErrorCode::ParseError:       return "ParseError";
    case ErrorCode::PlatformError:    return "PlatformError";
  }

  return "Unknown";
}

// Throws a JS Error annotated with `.code` (and, when available, `.path`/
// `.syscall`/`.nativeCode`) for a failed Result<T>/Result<void>. Per
// JsBindings.md, OutOfMemory is never routed through here -- callers use
// JS_ReportOutOfMemory directly, matching EnqueueMicrotask/SetTimeout/
// SetInterval above.
void ThrowJsError(JSContext* cx,
                                          const forge::core::Error& error,
                                          const char* context,
                                          const forge::core::Path* path) {
  const char* codeStr = ErrorCodeToString(error.Code());

  // forge::core::Error carries no free-text message (see Error.h -- just
  // Code()/NativeCode()), so the reported message is synthesized from
  // `context` and the mapped code string rather than inventing an
  // Error::Message() this codebase doesn't have.
  JS_ReportErrorUTF8(cx, "%s failed: %s", context, codeStr);

  if (!JS_IsExceptionPending(cx)) {
    // JS_ReportErrorUTF8 itself hit trouble (e.g. OOM formatting the
    // message) and left nothing pending to annotate further.
    return;
  }

  JS::RootedValue excVal(cx);
  if (!JS_GetPendingException(cx, &excVal) || !excVal.isObject()) {
    // Not an Error object we can annotate (shouldn't normally happen for
    // an exception JS_ReportErrorUTF8 itself just created) -- the base
    // message is still thrown, just without the extra properties below.
    return;
  }

  // Phase 8.4 (R24): the fields are defined as own data properties with
  // the exception taken off the context meanwhile. JS_SetProperty would
  // run a setter that a script placed on Error.prototype or
  // Object.prototype, in the middle of this native, with an exception
  // pending; JS_DefineProperty on an ordinary Error object runs no script.
  // Each field is best effort: a failure (out of memory) keeps the
  // original error rather than replacing it.
  JS::RootedObject excObj(cx, &excVal.toObject());
  JS_ClearPendingException(cx);

  auto defineValue = [&](const char* name, JS::HandleValue value) {
    if (!JS_DefineProperty(cx, excObj, name, value, JSPROP_ENUMERATE)) {
      JS_ClearPendingException(cx);
    }
  };

  if (JSString* codeJsStr = JS_NewStringCopyZ(cx, codeStr)) {
    JS::RootedValue codeVal(cx, JS::StringValue(codeJsStr));
    defineValue("code", codeVal);
  }

  if (JSString* syscallJsStr = JS_NewStringCopyZ(cx, context)) {
    JS::RootedValue syscallVal(cx, JS::StringValue(syscallJsStr));
    defineValue("syscall", syscallVal);
  }

  if (path != nullptr) {
    const forge::core::StringView pathView = path->View();
    if (JSString* pathJsStr = JS_NewStringCopyUTF8N(
            cx, JS::UTF8Chars(pathView.Data(), pathView.Size()))) {
      JS::RootedValue pathVal(cx, JS::StringValue(pathJsStr));
      defineValue("path", pathVal);
    }
  }

  if (error.Code() == forge::core::ErrorCode::PlatformError) {
    JS::RootedValue nativeCodeVal(cx, JS::NumberValue(error.NativeCode()));
    defineValue("nativeCode", nativeCodeVal);
  }

  JS_ClearPendingException(cx);  // an out-of-memory report from a string
  JS_SetPendingException(cx, excVal);
}

// Converts an arbitrary JS value to a forge::core::String via ToString
// semantics (matches JS::ToString(cx, value) + JS_EncodeStringToUTF8,
// the exact pattern Print() above already uses and this sandbox cannot
// re-verify beyond that existing, working precedent).
//
// Design note (documented here per the instruction to record any design
// clarification before implementing it -- this is a behavioral
// clarification of JsBindings.md's contract, not a change to any frozen
// signature): a Result<T> failure returned by this function, ToForgePath,
// or any future sibling never leaves its own exception pending on `cx` --
// JS_ClearPendingException is called internally on every failure path
// where an underlying JS:: call (JS::ToString, JS_EncodeStringToUTF8)
// may already have reported one. This lets every caller uniformly do
// `if (result.HasError()) { ThrowJsError(cx, result.Error(), ...); return
// false; }` on any Result<T> from this module without ever risking two
// exceptions pending at once. JsBindings.md's "Error Handling Policy"
// section is updated alongside this change to state the same thing.
forge::core::Result<forge::core::String> ToForgeString(JSContext* cx,
                                                        JS::HandleValue value) {
  JS::RootedString jsStr(cx, JS::ToString(cx, value));

  if (!jsStr) {
    JS_ClearPendingException(cx);
    return forge::core::Result<forge::core::String>(
        forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidArgument)});
  }

  JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, jsStr);

  if (!utf8) {
    JS_ClearPendingException(cx);
    return forge::core::Result<forge::core::String>(
        forge::core::Failure{forge::core::Error(forge::core::ErrorCode::OutOfMemory)});
  }

  // Treated as a null-terminated C string, same assumption Print() above
  // already relies on (printf("%s", bytes.get())) -- a JS string
  // containing an embedded U+0000 would be silently truncated here. Not
  // a new limitation introduced by this function, just inherited from
  // the same JS_EncodeStringToUTF8-based idiom already in production use
  // in this file.
  return forge::core::String::Create(forge::core::StringView(utf8.get()));
}

// (Phase 8.2: ToForgePath, which turned any value into a path with
// ToString, is gone. Filesystem natives get paths only through
// CheckPathArg, which accepts strings and asks Kavach; see rule R3.)

// Converts a forge::core::StringView to a new JS string (copies; does not
// take ownership of `text`'s storage, matching every JS_New*StringCopy*
// function's own documented convention in js/String.h, as opposed to
// JS_NewUCString's ownership-transferring convention).
//
// Returns nullptr on OOM. JS_NewStringCopyUTF8N is expected to report its
// own failure (standard JS_New*StringCopy* convention across this
// header), but since that isn't spelled out explicitly in js/String.h's
// comments and this sandbox has no way to compile-and-observe it
// directly, this reports defensively if the call somehow left nothing
// pending -- so a caller can always trust "null return means an
// exception is pending" without needing to know which of the two
// conventions is actually in effect.
JSString* FromForgeString(JSContext* cx,
                                          forge::core::StringView text) {
  JSString* result = JS_NewStringCopyUTF8N(
      cx, JS::UTF8Chars(text.Data(), text.Size()));

  if (!result && !JS_IsExceptionPending(cx)) {
    JS_ReportOutOfMemory(cx);
  }

  return result;
}

// Creates a new Uint8Array and copies `bytes`'s contents into it. Consumes
// `bytes` (taken by value; released when this function returns, per
// JsBindings.md's documented ownership: the JS engine owns the copy from
// here on, forge-core's Vector<u8> is not retained).
JSObject* Uint8ArrayFromBytes(JSContext* cx,
                                               forge::core::Vector<forge::core::u8> bytes) {
  const size_t length = static_cast<size_t>(bytes.Size());

  JS::RootedObject array(cx, JS_NewUint8Array(cx, length));

  if (!array) {
    // JS_NewUint8Array reports its own failure (OOM, or a RangeError if
    // `length` exceeds the engine's maximum typed array size).
    return nullptr;
  }

  if (length > 0) {
    JS::AutoCheckCannotGC nogc(cx);
    bool isSharedMemory = false;
    uint8_t* data = JS_GetUint8ArrayData(array, &isSharedMemory, nogc);

    // `array` was just created by JS_NewUint8Array immediately above, so
    // it is guaranteed to be a private, non-shared, non-detached buffer:
    // `data` is non-null and `isSharedMemory` is false here by
    // construction, not by runtime luck.
    memcpy(data, bytes.Data(), length);
  }

  return array;
}

// Reads a JS Uint8Array or ArrayBuffer's bytes without copying into a
// forge-core container -- the returned Span aliases the JS buffer's own
// storage and is valid only for the duration of the call site (per
// JsBindings.md), since a GC can move or (for a resizable/detachable
// buffer) invalidate the underlying storage afterward.
//
// Deliberately narrower than "any ArrayBufferView": JsBindings.md's
// Public API doc comment names exactly "a JS Uint8Array/ArrayBuffer",
// not every typed array element-type variant (Int16Array,
// Float64Array, etc.) -- so a caller passing e.g. a Float64Array gets
// InvalidArgument here rather than this function silently reinterpreting
// its bytes as raw uint8_t, which nothing in the frozen spec asked for.
forge::core::Result<forge::core::Span<const forge::core::u8>> AsByteSpan(
    JSContext* cx, JS::HandleValue value) {
  using ByteSpan = forge::core::Span<const forge::core::u8>;

  if (!value.isObject()) {
    return forge::core::Result<ByteSpan>(
        forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidArgument)});
  }

  JS::RootedObject obj(cx, &value.toObject());

  if (JS_IsUint8Array(obj)) {
    JS::AutoCheckCannotGC nogc(cx);
    bool isSharedMemory = false;
    uint8_t* data = JS_GetUint8ArrayData(obj, &isSharedMemory, nogc);
    const size_t length = JS_GetTypedArrayByteLength(obj);

    if (isSharedMemory || (length > 0 && data == nullptr)) {
      // A SharedArrayBuffer-backed view could be mutated by another
      // thread concurrently with whatever synchronous I/O the caller is
      // about to do with this span -- not safe to alias directly. A
      // null data pointer with nonzero length means a detached backing
      // buffer -- nothing valid to read.
      return forge::core::Result<ByteSpan>(
          forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidOperation)});
    }

    return forge::core::Result<ByteSpan>(
        ByteSpan(reinterpret_cast<const forge::core::u8*>(data), length));
  }

  if (JS::IsArrayBufferObject(obj)) {
    if (JS::IsDetachedArrayBufferObject(obj)) {
      return forge::core::Result<ByteSpan>(
          forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidOperation)});
    }

    JS::AutoCheckCannotGC nogc(cx);
    bool isSharedMemory = false;
    uint8_t* data = JS::GetArrayBufferData(obj, &isSharedMemory, nogc);
    const size_t length = JS::GetArrayBufferByteLength(obj);

    // isSharedMemory is always false for a plain (non-Shared)
    // ArrayBuffer per js/ArrayBuffer.h's own doc comment on
    // JS::GetArrayBufferData -- checked anyway rather than assumed.
    if (isSharedMemory) {
      return forge::core::Result<ByteSpan>(
          forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidOperation)});
    }

    return forge::core::Result<ByteSpan>(
        ByteSpan(reinterpret_cast<const forge::core::u8*>(data), length));
  }

  return forge::core::Result<ByteSpan>(
      forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidArgument)});
}

//==============================================================================

// Kavach glue (Phase 8.2)
//
// Every filesystem native gets its path through CheckPathArg, and every
// environment native goes through CheckEnvArg. Both throw the plain-language
// refusal from kavach::FormatDenial (permission UX spec, G38) as an Error
// with these extra properties:
//   code        "PermissionDenied", or "InvalidArgument" for an unusable path
//   permission  "read", "write", "env" or "env (every variable)"
//   op          the function, e.g. "fs.readFileSync"
//   path / variable  what was refused
// The properties are defined, not assigned, so no setter on
// Error.prototype can run.
//==============================================================================

void DefineStringOnPendingError(JSContext* cx, const char* name,
                                       forge::core::StringView value) {
  JS::RootedValue exception(cx);

  if (!JS_GetPendingException(cx, &exception) || !exception.isObject()) {
    return;
  }

  // Defined (never set) with the exception off the context, so no setter
  // runs (R24); the original error stays pending whatever happens.
  JS::RootedObject error(cx, &exception.toObject());
  JS_ClearPendingException(cx);
  JS::RootedString str(cx, JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(value.Data(), value.Size())));

  if (str) {
    JS::RootedValue strValue(cx, JS::StringValue(str));
    (void)JS_DefineProperty(cx, error, name, strValue, JSPROP_ENUMERATE);
  }

  JS_ClearPendingException(cx);
  JS_SetPendingException(cx, exception);
}

bool ThrowKavachDenial(JSContext* cx, forge::kavach::Op op,
                              const forge::kavach::Denial& denial) {
  forge::core::String message;
  forge::kavach::FormatDenial(op, denial, message);

  if (message.Empty()) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  JS_ReportErrorUTF8(cx, "%s", message.CStr());

  if (!JS_IsExceptionPending(cx)) {
    return false;
  }

  const forge::kavach::OpInfo& info = forge::kavach::GetOpInfo(op);
  const bool invalidPath = denial.kind == forge::kavach::Denial::Kind::InvalidPath;
  const bool isEnv = info.permission == forge::kavach::Permission::Env ||
                     info.permission == forge::kavach::Permission::EnvAll;

  DefineStringOnPendingError(cx, "code", invalidPath ? "InvalidArgument" : "PermissionDenied");
  // For filesystem ops, the access that was refused (Phase 8.3: fs.copyFile
  // reads one path and writes another).
  const char* permission = isEnv ? forge::kavach::PermissionName(info.permission)
                           : denial.access == forge::kavach::Access::Write ? "write"
                                                                           : "read";
  DefineStringOnPendingError(cx, "permission", permission);
  DefineStringOnPendingError(cx, "op", info.name);
  DefineStringOnPendingError(cx, isEnv ? "variable" : "path", denial.target.View());
  return false;
}

// Throws a TypeError for a wrong argument type, with code "InvalidArgument"
// like Forge's other argument errors.
bool ThrowArgumentTypeError(JSContext* cx, forge::kavach::Op op, const char* what) {
  char message[160];
  snprintf(message, sizeof(message), "%s: %s", forge::kavach::GetOpInfo(op).name, what);
  ThrowTypeError(cx, message);
  DefineStringOnPendingError(cx, "code", "InvalidArgument");
  return false;
}

// The only way a filesystem native gets a path (rule R3). Only a string is
// accepted: no ToString, so an object cannot choose its own path through
// toString(), and no Uint8Array or URL forms until they are designed.
forge::core::Result<forge::kavach::CheckedPath> CheckPathArg(JSContext* cx,
                                                             forge::kavach::Op op,
                                                             JS::HandleValue value) {
  const forge::kavach::Access access =
      forge::kavach::GetOpInfo(op).permission == forge::kavach::Permission::Write
          ? forge::kavach::Access::Write
          : forge::kavach::Access::Read;
  return CheckPathArg(cx, op, access, value);
}

forge::core::Result<forge::kavach::CheckedPath> CheckPathArg(JSContext* cx,
                                                             forge::kavach::Op op,
                                                             forge::kavach::Access access,
                                                             JS::HandleValue value) {
  using Checked = forge::kavach::CheckedPath;
  const forge::core::Failure failure{forge::core::Error(forge::core::ErrorCode::InvalidArgument)};

  if (!value.isString()) {
    ThrowArgumentTypeError(cx, op, "the path must be a string");
    return forge::core::Result<Checked>(failure);
  }

  forge::core::String utf8;

  if (!forge::runtime::AppendStringUtf8(cx, value.toString(), utf8)) {
    return forge::core::Result<Checked>(failure);
  }

  forge::kavach::Denial denial;
  forge::core::Result<Checked> checked = forge::kavach::CheckPath(op, access, utf8.View(), denial);

  if (checked.HasError()) {
    ThrowKavachDenial(cx, op, denial);
  }

  return checked;
}


bool TakePendingException(JSContext* cx, JS::MutableHandleValue exception) {
  if (!JS_GetPendingException(cx, exception)) {
    exception.setUndefined();
    return false;
  }

  JS_ClearPendingException(cx);
  return true;
}

} // namespace forge::runtime
