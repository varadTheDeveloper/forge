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

#include "js/Class.h"
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

/// Defines the web APIs on `global`, the current realm's global object.
/// Call once per realm, after JS::InitRealmStandardClasses and before any
/// other script runs in it. Fails (with an exception pending) if the script
/// does not define exactly kWebApiGlobalNames.
[[nodiscard]]
bool DefineWebApis(
    JSContext* cx,
    JS::HandleObject global);

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
