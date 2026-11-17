#pragma once

// Web-standard APIs (Phase 10, PHASE10_DESIGN.md, decision D27).
//
// The WebIDL layer of each API (argument conversion, brand checks,
// prototypes, internal state) is JavaScript: runtime/WebApis.js, embedded by
// tools/gen-webapis.py. The byte work (UTF-8, base64, the other decoders) is
// portable C++ in runtime/Encoding, reached through a "natives" object that
// only that script ever holds. DefineWebApis runs the script in a realm
// right after the engine's standard classes and before any other script, so
// the script captures the built-ins it relies on before user code can
// change them.
//
// The globals it defines are treated as standard globals: the lockdown walk
// starts from them too, so in a package compartment they are frozen like
// every other intrinsic, and one crossing compartments is replaced by the
// receiving side's own (runtime/Compartments.cpp). None of them needs a
// permission (kavach/Ops.h rows with Permission::None).
//
// Phase 10.2: the global object is an EventTarget, and an uncaught error
// or an unhandled rejection is first dispatched as an "error" or
// "unhandledrejection" event at the PROGRAM's global object (never a
// package's: a package must not be able to hide another package's errors,
// D28). It is fatal only if no listener cancels it.

#include "js/Class.h"
#include "js/Exception.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

#include "forge-core/Types.h"

namespace forge::runtime
{

/// The reserved slot of Forge's global objects that holds the realm's web
/// API internals (the DOMException records, the members table). forge.cpp's
/// global class reserves it.
constexpr uint32_t kWebInternalsSlot = JSCLASS_GLOBAL_SLOT_COUNT + 1;

/// The globals DefineWebApis defines. They are walk roots for the lockdown
/// (forge.cpp passes them to SetStandardGlobalNames with the engine's own).
extern const char* const kWebApiGlobalNames[];
extern const forge::core::Size kWebApiGlobalCount;

enum class WebRealm
{
    Program,  // the program's realm: receives the uncaught-error events
    Package,  // a package compartment
};

/// Defines the web APIs on `global`, the current realm's global object.
/// Call once per realm, after JS::InitRealmStandardClasses and before any
/// other script runs in it. Fails (with an exception pending) if the script
/// does not define exactly kWebApiGlobalNames. Exactly one realm is
/// WebRealm::Program, and it must be defined first.
[[nodiscard]]
bool DefineWebApis(
    JSContext* cx,
    JS::HandleObject global,
    WebRealm kind);

/// Releases what DefineWebApis kept. Call before the context is destroyed.
void ShutdownWebApis();

/// The HTML "report an exception" steps: dispatches an ErrorEvent (message,
/// filename, line and column from the error report, error = the value) at
/// the program's global object. If no listener cancels it, the exception is
/// uncaught: HostReportUncaught prints it and ends the program. Runs in any
/// realm; nothing is left pending. `context` names where it happened.
void ReportException(
    JSContext* cx,
    const JS::ExceptionStack& exception,
    const char* context);

/// The HTML "notify about rejected promises" steps for one rejected
/// promise with no handler (called in the promise's realm): dispatches
/// "unhandledrejection" at the program's global object. `*handled` is set
/// if a listener cancelled it; the rejection is then not fatal, and a
/// handler attached later fires "rejectionhandled" (FireRejectionHandled).
void FireUnhandledRejection(
    JSContext* cx,
    JS::HandleObject promise,
    bool* handled);

/// A handler was attached to `promise` after it was reported (called in
/// the promise's realm): dispatches "rejectionhandled" if its
/// "unhandledrejection" event was cancelled.
void FireRejectionHandled(
    JSContext* cx,
    JS::HandleObject promise);

/// Implemented by forge.cpp, which owns exit and error output: prints
/// `exception` as uncaught in `context` and ends the program with exit code
/// 1 (or, after Forge.exit(), just stops).
void HostReportUncaught(
    JSContext* cx,
    const JS::ExceptionStack& exception,
    const char* context);

/// For console output (runtime/Inspect.cpp): if `obj`, an object of the
/// current realm, is a DOMException, sets `*found` and its name and message.
/// Runs no script.
[[nodiscard]]
bool GetDomExceptionFields(
    JSContext* cx,
    JS::HandleObject obj,
    JS::MutableHandleValue name,
    JS::MutableHandleValue message,
    bool* found);

} // namespace forge::runtime
