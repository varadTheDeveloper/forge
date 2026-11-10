#pragma once

// Value formatting for console output and error reports (Phase 8).
//
// The output follows Node's util.inspect closely enough to be familiar:
//   { a: 1, b: 'x' }   [ 1, 2, 3 ]   Map(1) { 'k' => 1 }   [Function: f]
//
// Rule: inspecting a value NEVER runs JavaScript code. Getters are shown as
// [Getter], Proxies as [Proxy], and no toString, valueOf, Symbol.toPrimitive
// or iterator is ever called. console.log(x) therefore cannot be used to
// make code run "on behalf of" whoever logs a value (this matters once
// packages are isolated from each other in Phase 9).

#include "js/CallArgs.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"

#include "forge-core/String.h"

namespace forge::runtime
{

struct InspectOptions
{
    /// Objects nested deeper than this print as [Object], [Array], ...
    unsigned depth = 2;

    /// A string at the top level prints without quotes (console.log("a")).
    bool rawTopLevelStrings = true;
};

/// Appends a readable form of `value` to `out`. Returns false only if the
/// engine reported a failure (an exception, usually out of memory, is then
/// pending) or `out` could not grow.
[[nodiscard]]
bool AppendInspected(
    JSContext* cx,
    JS::HandleValue value,
    forge::core::String& out,
    const InspectOptions& options = InspectOptions());

/// The WHATWG console "Formatter" and "Logger" steps: formats
/// args[first, args.length()) into one string, applying %s %d %i %f %o %O
/// %c and %% when the first of them is a string, and joining the rest with
/// spaces. Like AppendInspected, never runs JavaScript code.
[[nodiscard]]
bool AppendFormattedArgs(
    JSContext* cx,
    const JS::CallArgs& args,
    unsigned first,
    forge::core::String& out);

/// Appends a JS string to `out` as UTF-8 (unpaired surrogates become
/// U+FFFD; embedded NUL characters are kept). Returns false with an
/// exception pending on failure, including running out of memory.
[[nodiscard]]
bool AppendStringUtf8(
    JSContext* cx,
    JSString* str,
    forge::core::String& out);

} // namespace forge::runtime
