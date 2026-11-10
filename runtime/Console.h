#pragma once

// The WHATWG `console` object, written in C++ (Phase 8).
//
// log, info, debug, dirxml and table write to standard output; warn,
// error, trace and assert write to standard error. Values are formatted by
// runtime/Inspect.h, which never runs JavaScript code. console.table is
// not implemented yet and prints like console.log.

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

#include "forge-core/StringView.h"

namespace forge::runtime
{

/// Defines globalThis.console. Returns false with an exception pending on
/// failure.
[[nodiscard]]
bool DefineConsole(
    JSContext* cx,
    JS::HandleObject global);

/// Writes `text` plus a newline to standard error, not indented. Used for
/// runtime messages such as uncaught exceptions, so they show correctly on
/// a console.
void WriteErrorLine(
    forge::core::StringView text);

} // namespace forge::runtime
