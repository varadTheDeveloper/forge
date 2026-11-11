#pragma once

// Shared JavaScript-binding helpers (Phase 8.3): converting between forge-core
// and JS values, throwing Forge's errors, and the Kavach argument checks.
// Used by forge.cpp and runtime/Fs.cpp.

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"

#include "forge-core/Error.h"
#include "forge-core/Path.h"
#include "forge-core/Result.h"
#include "forge-core/Span.h"
#include "forge-core/String.h"
#include "forge-core/StringView.h"
#include "forge-core/Types.h"
#include "forge-core/memory/Vector.h"
#include "kavach/Kavach.h"
#include "kavach/Ops.h"

namespace forge::runtime
{

/// ErrorCode -> the `code` string on Forge's errors ("NotFound", ...).
const char* ErrorCodeToString(
    forge::core::ErrorCode code);

/// Throws an Error "<context> failed: <code>" with `code`, `syscall`, and
/// (when given) `path` properties, plus `nativeCode` for PlatformError.
void ThrowJsError(
    JSContext* cx,
    const forge::core::Error& error,
    const char* context,
    const forge::core::Path* path = nullptr);

/// JS::ToString + UTF-8. Never leaves an exception pending on failure.
forge::core::Result<forge::core::String> ToForgeString(
    JSContext* cx,
    JS::HandleValue value);

/// A new JS string from UTF-8; null (with an exception pending) on failure.
JSString* FromForgeString(
    JSContext* cx,
    forge::core::StringView text);

/// A new Uint8Array holding `bytes`; null (with an exception pending) on
/// failure.
JSObject* Uint8ArrayFromBytes(
    JSContext* cx,
    forge::core::Vector<forge::core::u8> bytes);

/// The bytes of a Uint8Array or ArrayBuffer, aliased; valid only until the
/// next call that can run script or GC.
forge::core::Result<forge::core::Span<const forge::core::u8>> AsByteSpan(
    JSContext* cx,
    JS::HandleValue value);

/// Throw a TypeError / RangeError with `message`; always return false.
bool ThrowTypeError(
    JSContext* cx,
    const char* message);

bool ThrowRangeError(
    JSContext* cx,
    const char* message);

/// Defines (not assigns) a string property on the pending exception.
void DefineStringOnPendingError(
    JSContext* cx,
    const char* name,
    forge::core::StringView value);

/// Throws Kavach's refusal message with code/permission/op/path fields.
bool ThrowKavachDenial(
    JSContext* cx,
    forge::kavach::Op op,
    const forge::kavach::Denial& denial);

/// TypeError "<op>: <what>" with code "InvalidArgument".
bool ThrowArgumentTypeError(
    JSContext* cx,
    forge::kavach::Op op,
    const char* what);

/// The only way a filesystem native gets a path (rule R3): a string,
/// checked by Kavach for the op's own access. Throws on failure.
forge::core::Result<forge::kavach::CheckedPath> CheckPathArg(
    JSContext* cx,
    forge::kavach::Op op,
    JS::HandleValue value);

/// The same for an explicit access (fs.copyFile: Read for the source,
/// Write for the destination).
forge::core::Result<forge::kavach::CheckedPath> CheckPathArg(
    JSContext* cx,
    forge::kavach::Op op,
    forge::kavach::Access access,
    JS::HandleValue value);

/// Moves the pending exception into `exception` (undefined if none).
bool TakePendingException(
    JSContext* cx,
    JS::MutableHandleValue exception);

} // namespace forge::runtime
