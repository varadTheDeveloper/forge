#pragma once

// Compartments (Phase 9.3, PHASE9_DESIGN.md section 5, decision D25).
//
// The program runs in the app compartment. Every dependency runs in another
// compartment of the same zone; in 9.3 that is one shared "pure"
// compartment, because no dependency can hold a permission before 9.4.
//
// What this file enforces:
//   - Lockdown: in a dependency compartment every intrinsic (every object
//     reachable from the standard globals, plus the hidden prototypes such as
//     %ArrayIteratorPrototype%) is frozen by a C++ walk, and so are its
//     global object and Forge's objects on it. A few data properties are
//     turned into accessors first, so assigning `toString`, `constructor`,
//     `name` or `message` to an object that inherits them keeps working
//     (the "override mistake").
//   - The wrapper policy (JS_SetWrapObjectCallbacks): ordinary objects and
//     functions cross as transparent cross-compartment wrappers; Forge's own
//     functions and namespace objects and any global object never cross (a
//     TypeError); an intrinsic is replaced by the receiving compartment's
//     own intrinsic of the same identity, so a package can never hold the
//     program's Object.prototype, Function constructor or eval.
//   - Dependencies cannot turn strings into code: IsDependencyRealm drives
//     the engine's code-generation callback in forge.cpp.

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

#include "forge-core/String.h"
#include "forge-core/Types.h"
#include "forge-core/memory/Vector.h"

namespace forge::runtime
{

/// Compartment numbers used by runtime/Modules.cpp.
constexpr forge::core::Size kAppCompartment = 0;
constexpr forge::core::Size kPureCompartment = 1;

/// Implemented by forge.cpp, which owns the global object's class and the
/// natives that hold no authority.
struct CompartmentHooks
{
    /// A new global object of Forge's global class in a new compartment of
    /// `sameZoneAs`'s zone, with the same realm options as the program's.
    /// Standard classes are not initialized yet. Null with an exception
    /// pending on failure.
    JSObject* (*newGlobal)(JSContext* cx, JS::HandleObject sameZoneAs) = nullptr;

    /// Defines the globals a dependency gets: console, the timer functions
    /// and queueMicrotask. Called inside the new global's realm.
    bool (*defineDependencyGlobals)(JSContext* cx, JS::HandleObject global) = nullptr;
};

/// Installs the wrapper policy and starts tracing. Call once, right after
/// the context is created.
[[nodiscard]]
bool InitCompartments(
    JSContext* cx,
    const CompartmentHooks& hooks);

/// Drops every compartment record. Call before the context is destroyed.
void ShutdownCompartments(
    JSContext* cx);

/// The names of the standard globals, captured by forge.cpp right after the
/// program's global got its standard classes and before Forge added its own.
/// The lockdown walk starts from these names, so it never mistakes a Forge
/// object for an intrinsic.
[[nodiscard]]
bool SetStandardGlobalNames(
    const forge::core::Vector<forge::core::String>& names);

/// Records the program's global (compartment kAppCompartment). With
/// `isolate` (the program has dependencies) or `freeze` (the
/// --frozen-intrinsics option) its intrinsics are walked now, before any of
/// the program's code runs; with `freeze` they are also frozen. Call inside
/// the program's realm.
[[nodiscard]]
bool PrepareAppRealm(
    JSContext* cx,
    JS::HandleObject global,
    bool isolate,
    bool freeze);

/// The global object of compartment `index`, creating the pure compartment
/// on first use. Null with an exception pending on failure.
JSObject* CompartmentGlobal(
    JSContext* cx,
    forge::core::Size index);

/// True if the current realm belongs to a dependency.
bool IsDependencyRealm(
    JSContext* cx);

/// Marks `obj`, an object of the current realm, as one of Forge's own
/// objects, which never cross into another compartment.
[[nodiscard]]
bool MarkHostObject(
    JSContext* cx,
    JS::HandleObject obj);

/// The object whose realm must be entered to call `callable`: the callable
/// itself, or for a cross-compartment wrapper the global of its compartment.
JSObject* RealmEntryFor(
    JSObject* callable);

} // namespace forge::runtime
