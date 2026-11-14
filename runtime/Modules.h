#pragma once

// ES modules (Phase 9.1, PHASE9_DESIGN.md section 3, decisions D22 and D23).
//
// Every program file is a module. The engine's module load hook is the only
// way code is loaded: static imports load synchronously while the graph is
// built, dynamic import() is deferred to a promise job. Each file is read
// through kavach/SecureFs from a CheckedPath made for Op::ModuleImport, so
// it must lie inside the program's module root (set up by
// kavach::SetUpProgramModules) before and after links are followed.

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

#include "forge-core/StringView.h"

namespace forge::runtime
{

/// Installs the module load and import.meta hooks and starts tracing the
/// module registry. Call once, right after the context is created.
[[nodiscard]]
bool InitModules(
    JSContext* cx);

/// Drops the module registry. Call before the context is destroyed.
void ShutdownModules(
    JSContext* cx);

enum class EntryResult
{
    Running,  // loaded and evaluated (top-level await may still be pending)
    Threw,    // failed: an exception is pending, or none after an uncatchable
              // error such as Forge.exit()
};

/// Loads, links and evaluates the entry module at `entryPath` (canonical,
/// resolved; from kavach::SetUpProgramModules). If the module's top-level
/// await later rejects, the rejection is reported through HostReportFatal.
EntryResult RunEntryModule(
    JSContext* cx,
    forge::core::StringView entryPath);

// Implemented by forge.cpp, which owns exit and error reporting.
/// True once Forge.exit() or a signal asked the program to stop.
bool HostExitRequested(JSContext* cx);
/// Reports the pending exception as uncaught in `context` and stops the
/// program with exit code 1.
void HostReportFatal(JSContext* cx, const char* context);

} // namespace forge::runtime
