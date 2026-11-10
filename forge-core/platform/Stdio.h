#pragma once

// Standard output, standard error and console signals (Phase 8).
//
// Text is UTF-8 everywhere in Forge. When a stream is a console window the
// text is converted to UTF-16 and written with WriteConsoleW, so every
// character shows correctly whatever the console code page is. When a
// stream is a pipe or a file the UTF-8 bytes are written unchanged with
// WriteFile. Before Phase 8 everything went through the C runtime's
// printf, which garbles non-ASCII text on a console.

#if !defined(_WIN32)
#error "forge::core::platform::Stdio: Windows-only."
#endif

#include "../Result.h"
#include "../Types.h"
#include "../memory/ResultVoid.h"

namespace forge::core::platform
{

enum class StdStream
{
    Out,
    Err
};

/// Writes all `size` bytes of UTF-8 to the stream. On a console, invalid
/// UTF-8 shows as U+FFFD. A stream that does not exist (a process started
/// without standard handles) swallows the bytes and succeeds. Flushes the
/// C runtime's stdout and stderr buffers first, so output written with
/// printf earlier stays in order.
[[nodiscard]]
Result<void> WriteStd(
    StdStream stream,
    const char* data,
    Size size) noexcept;

/// True if the stream is a console window (not a pipe, file or nothing).
[[nodiscard]]
bool IsTerminal(
    StdStream stream) noexcept;

enum class ConsoleSignal
{
    Interrupt,  // Ctrl+C       (SIGINT)
    Break       // Ctrl+Break   (SIGBREAK)
};

/// Called on a thread the system creates for the purpose, never on the
/// main thread. Returns true if the signal was handled; false lets the
/// default action (ending the process) happen.
using ConsoleSignalCallback = bool (*)(ConsoleSignal signal) noexcept;

/// Installs the process-wide console control handler. Only Ctrl+C and
/// Ctrl+Break reach the callback; closing the console window, logging off
/// and shutting down keep the default behaviour.
[[nodiscard]]
Result<void> InstallConsoleSignalHandler(
    ConsoleSignalCallback callback) noexcept;

/// Removes the handler. After this returns no new call to the callback
/// starts (a call already running may still finish).
void RemoveConsoleSignalHandler() noexcept;

/// Ends the process at once with `exitCode`, without running any more code
/// in this process (no destructors, no atexit handlers, no flushing).
[[noreturn]]
void TerminateProcessNow(
    u32 exitCode) noexcept;

} // namespace forge::core::platform
